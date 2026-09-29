#include "db/db_impl.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "common/crash_point.hpp"
#include "common/file_io.hpp"
#include "common/file_names.hpp"
#include "common/internal_key.hpp"
#include "manifest/version.hpp"
#include "minilsm/format_limits.hpp"
#include "sstable/sstable_builder.hpp"

namespace minilsm {

namespace {

FileMetaData MakeFlushedFileMetadata(
    std::uint64_t file_number,
    std::uint64_t file_size,
    const MemTable& memtable) {
    const MemTable::RecordMap& records = memtable.records();
    const InternalKey& first = records.begin()->first;
    const InternalKey& last = records.rbegin()->first;

    FileMetaData metadata;
    metadata.file_number = file_number;
    metadata.level = 0U;
    metadata.file_size = file_size;
    metadata.entry_count = records.size();
    metadata.smallest_sequence = first.sequence_number;
    metadata.largest_sequence = first.sequence_number;
    metadata.smallest_user_key = first.user_key;
    metadata.largest_user_key = last.user_key;
    metadata.smallest_internal_key = first;
    metadata.largest_internal_key = last;
    for (const auto& [internal_key, value] : records) {
        static_cast<void>(value);
        if (internal_key.sequence_number < metadata.smallest_sequence) {
            metadata.smallest_sequence = internal_key.sequence_number;
        }
        if (internal_key.sequence_number > metadata.largest_sequence) {
            metadata.largest_sequence = internal_key.sequence_number;
        }
    }
    return metadata;
}

Status BuildSstableFromMemTable(
    const Options& options,
    const std::filesystem::path& temporary_path,
    const MemTable& memtable) {
    std::unique_ptr<SstableBuilder> builder;
    Status status = SstableBuilder::Create(
        temporary_path,
        options.sstable_block_target_bytes,
        &builder, options.prefix_compression);
    if (!status.ok()) {
        return status;
    }
    for (const auto& [internal_key, value] : memtable.records()) {
        status = builder->Add(internal_key, value);
        if (!status.ok()) {
            return status;
        }
    }
    return builder->Finish();
}

}  // namespace

DBImpl::DBImpl(Options options, std::filesystem::path directory, RecoveredDatabaseState state)
    : options_(options), directory_(std::move(directory)),
      version_(std::move(state.version)), memtable_(std::move(state.memtable)),
      immutable_(std::move(state.immutable)), wal_writer_(std::move(state.wal_writer)),
      directory_lock_(std::move(state.directory_lock)), next_sequence_(state.next_sequence),
      block_cache_(std::make_shared<BlockCache>(options.block_cache_bytes)),
      table_cache_(directory_, options.table_cache_entries, block_cache_) {}

DBImpl::~DBImpl() { StopWorker(); }

Status DBImpl::Open(const Options& options, const std::filesystem::path& directory,
                    std::unique_ptr<DBImpl>* db) {
    if (db == nullptr) return Status::InvalidArgument("DB open received a null output");
    Status status = options.Validate();
    if (!status.ok()) return status;
    RecoveredDatabaseState state;
    status = OpenOrCreateDatabaseState(options, directory, &state);
    if (!status.ok()) return status;
    auto opened = std::unique_ptr<DBImpl>(new DBImpl(options, directory, std::move(state)));
    if (options.background_maintenance) {
        opened->worker_ = std::thread(&DBImpl::BackgroundLoop, opened.get());
    } else if (opened->immutable_) {
        std::unique_lock lock(opened->mutex_);
        status = opened->FlushMemTable(lock);
        if (!status.ok()) return status;
    }
    *db = std::move(opened);
    return Status::Ok();
}

Status DBImpl::Put(std::string_view key, std::string_view value) {
    WriteBatch batch;
    batch.Put(key, value);
    return Write(WriteOptions{}, batch);
}

Status DBImpl::Delete(std::string_view key) {
    WriteBatch batch;
    batch.Delete(key);
    return Write(WriteOptions{}, batch);
}

Status DBImpl::Write(const WriteOptions& options, const WriteBatch& batch) {
    std::lock_guard writer(writer_mutex_);
    std::unique_lock lock(mutex_);
    if (closed_) return Status::IOError("database is closed");
    if (!background_error_.ok()) return background_error_;
    if (batch.empty()) return Status::Ok();
    if (!next_sequence_ || batch.size() - 1U > std::numeric_limits<std::uint64_t>::max() - *next_sequence_) {
        return Status::IOError("sequence-number space is exhausted");
    }
    for (const auto& mutation : batch.mutations()) {
        if (mutation.key.size() > kMaxUserKeyBytes || mutation.value.size() > kMaxValueBytes) {
            return Status::InvalidArgument("batch key or value exceeds format limit");
        }
    }
    if (memtable_.approximate_memory_usage() >= options_.memtable_bytes_limit) {
        if (options_.background_maintenance) {
            if (immutable_ || maintenance_busy_) ++read_counters_.write_stalls;
            condition_.wait(lock, [&] { return (!immutable_ && !maintenance_busy_) || !background_error_.ok(); });
            if (!background_error_.ok()) return background_error_;
            Status status = RotateMemTable();
            if (!status.ok()) return status;
        } else {
            Status status = FlushMemTable(lock);
            if (!status.ok()) return status;
        }
    }
    const std::uint64_t first = *next_sequence_;
    Status status = wal_writer_->AppendBatch(first, batch, options.sync);
    if (!status.ok()) return status;
    CrashPoint("wal_after_sync_before_memtable");
    std::uint64_t sequence = first;
    for (const auto& mutation : batch.mutations()) {
        status = memtable_.Add({mutation.key, sequence,
            mutation.type == MutationType::kPut ? RecordType::kValue : RecordType::kTombstone}, mutation.value);
        if (!status.ok()) { background_error_ = status; return status; }
        ++sequence; // unsigned wrap means this batch used UINT64_MAX
    }
    next_sequence_ = sequence == 0U ? std::nullopt : std::optional<std::uint64_t>(sequence);
    if (memtable_.approximate_memory_usage() >= options_.memtable_bytes_limit) {
        if (!options_.background_maintenance) return FlushMemTable(lock);
        if (!immutable_ && !maintenance_busy_) return RotateMemTable();
    }
    return Status::Ok();
}

Status DBImpl::RotateMemTable() {
    // Seal before publishing both logs. No acknowledged async write is lost
    // if the machine crashes immediately after switching CURRENT.
    Status status = wal_writer_->Sync();
    if (!status.ok()) return status;
    status = CleanupObsoleteFiles(directory_, version_);
    if (!status.ok()) return status;
    Version next = version_;
    std::uint64_t number = 0U;
    status = AllocateFileNumber(&next, &number);
    if (status.ok()) status = AllocateFileNumber(&next, &next.manifest_file_number);
    version_.next_file_number = next.next_file_number;
    if (!status.ok()) return status;
    std::string name;
    status = MakeWalFileName(number, false, &name);
    std::unique_ptr<WalWriter> writer;
    if (status.ok()) status = WalWriter::CreateNew(directory_ / name, number, &writer);
    if (status.ok()) status = SyncDirectory(directory_);
    next.immutable_wal_file_number = version_.active_wal_file_number;
    next.active_wal_file_number = number;
    if (status.ok()) status = WriteManifestSnapshotFile(directory_, next, false);
    if (!status.ok()) return status;
    bool replaced = false;
    const bool fail_sync = fail_next_current_sync_for_testing_;
    fail_next_current_sync_for_testing_ = false;
    status = InstallCurrentFile(directory_, next.manifest_file_number, &replaced, false, fail_sync);
    if (!replaced) return status;
    immutable_ = std::make_unique<MemTable>(std::move(memtable_));
    memtable_ = MemTable{};
    version_ = std::move(next);
    const Status close_status = wal_writer_->Close();
    wal_writer_ = std::move(writer);
    if (!status.ok()) background_error_ = status;
    else if (!close_status.ok()) background_error_ = close_status;
    if (background_error_.ok()) CrashPoint("rotation_after_current_installation");
    condition_.notify_all();
    return background_error_;
}

Status DBImpl::FlushMemTable(std::unique_lock<std::mutex>& lock) {
    const bool flushing_immutable = immutable_ != nullptr;
    const MemTable& source = flushing_immutable ? *immutable_ : memtable_;
    if (source.empty()) {
        return Status::Ok();
    }

    // A failed earlier attempt can leave only unreferenced artifacts because
    // CURRENT was not changed. Remove those before allocating another group
    // of file numbers before the next serialized maintenance job.
    Status status = CleanupObsoleteFiles(directory_, version_);
    if (!status.ok()) {
        return status;
    }

    Version next_version = version_;
    std::uint64_t sstable_file_number = 0U;
    std::uint64_t new_wal_file_number = 0U;
    std::uint64_t manifest_file_number = 0U;
    status = AllocateFileNumber(&next_version, &sstable_file_number);
    if (status.ok() && !flushing_immutable) {
        status = AllocateFileNumber(&next_version, &new_wal_file_number);
    }
    if (status.ok()) {
        status = AllocateFileNumber(&next_version, &manifest_file_number);
    }
    if (!status.ok()) {
        return status;
    }
    // Keep attempted allocations monotonic for retries in this process. If a
    // crash leaves numbered artifacts, recovery separately raises the loaded
    // manifest allocator past those names before deleting them.
    version_.next_file_number = next_version.next_file_number;

    std::string temporary_sstable_name;
    std::string final_sstable_name;
    status = MakeSstableFileName(
        sstable_file_number, true, &temporary_sstable_name);
    if (status.ok()) {
        status = MakeSstableFileName(
            sstable_file_number, false, &final_sstable_name);
    }
    if (!status.ok()) {
        return status;
    }
    bool final_sstable_exists = false;
    status = FileExists(
        directory_ / final_sstable_name, &final_sstable_exists);
    if (!status.ok()) {
        return status;
    }
    if (final_sstable_exists) {
        return Status::IOError(
            "refusing to overwrite existing SSTable '" +
            final_sstable_name + "'");
    }

    // Phase 2-3: build and fsync the temporary SSTable, rename it to its
    // final numbered name, then make that directory entry durable.
    if (fail_next_sstable_build_for_testing_) {
        fail_next_sstable_build_for_testing_ = false;
        return Status::IOError("simulated SSTable build failure");
    }
    lock.unlock();
    status = BuildSstableFromMemTable(options_, directory_ / temporary_sstable_name, source);
    lock.lock();
    if (!status.ok()) {
        return status;
    }
    CrashPoint("flush_after_sstable_temp_file_completion");
    status = AtomicRename(
        directory_ / temporary_sstable_name,
        directory_ / final_sstable_name);
    if (!status.ok()) {
        return status;
    }
    CrashPoint("flush_after_sstable_final_rename");
    status = SyncDirectory(directory_);
    if (!status.ok()) {
        return status;
    }

    std::uint64_t sstable_file_size = 0U;
    status = GetFileSize(
        directory_ / final_sstable_name, &sstable_file_size);
    if (!status.ok()) {
        return status;
    }
    FileMetaData new_file = MakeFlushedFileMetadata(
        sstable_file_number, sstable_file_size, source);
    std::unique_ptr<SstableReader> new_sstable_reader;
    lock.unlock();
    status = ValidateSstableFile(directory_, &new_file, &new_sstable_reader);
    lock.lock();
    if (!status.ok()) {
        return status;
    }

    // Phase 4: create and fsync the new empty WAL before any manifest can
    // name it, then sync its new directory entry.
    std::unique_ptr<WalWriter> new_wal_writer;
    if (!flushing_immutable) {
        std::string new_wal_name;
        status = MakeWalFileName(new_wal_file_number, false, &new_wal_name);
        if (!status.ok()) return status;
        if (fail_next_new_wal_creation_for_testing_) {
            fail_next_new_wal_creation_for_testing_ = false;
            return Status::IOError("simulated new WAL creation failure");
        }
        status = WalWriter::CreateNew(directory_ / new_wal_name, new_wal_file_number, &new_wal_writer);
        if (!status.ok()) return status;
        CrashPoint("flush_after_new_wal_creation");
        status = SyncDirectory(directory_);
        if (!status.ok()) return status;
        next_version.active_wal_file_number = new_wal_file_number;
    }
    next_version.immutable_wal_file_number = 0U;
    next_version.manifest_file_number = manifest_file_number;
    next_version.last_sequence = new_file.largest_sequence;
    next_version.levels[0].push_back(new_file);

    // Phase 5-6: encode the complete snapshot, fsync its temporary file,
    // rename it to the final MANIFEST name, and sync the directory.
    if (fail_next_manifest_write_for_testing_) {
        fail_next_manifest_write_for_testing_ = false;
        return Status::IOError("simulated MANIFEST write failure");
    }
    const bool fail_manifest_rename =
        fail_next_manifest_rename_for_testing_;
    fail_next_manifest_rename_for_testing_ = false;
    status = WriteManifestSnapshotFile(
        directory_, next_version, fail_manifest_rename);
    if (!status.ok()) {
        return status;
    }

    // Phase 7: CURRENT replacement is the logical commit point. If its
    // following directory sync fails, the visible pathname already selects
    // the new state, so in-memory state must still switch to match it.
    const bool fail_current_install =
        fail_next_current_install_for_testing_;
    fail_next_current_install_for_testing_ = false;
    const bool fail_sync = fail_next_current_sync_for_testing_;
    fail_next_current_sync_for_testing_ = false;
    bool current_replaced = false;
    const Status current_status = InstallCurrentFile(
        directory_,
        manifest_file_number,
        &current_replaced,
        fail_current_install, fail_sync);
    if (!current_replaced) {
        return current_status;
    }

    // Phase 8: only after CURRENT changed do we clear the flushed memtable and
    // switch the active writer/read set to the newly authoritative files.
    version_ = std::move(next_version);
    ++read_counters_.flush_count;
    Status close_status;
    if (flushing_immutable) {
        immutable_.reset();
    } else {
        close_status = wal_writer_->Close();
        wal_writer_ = std::move(new_wal_writer);
        memtable_.Clear();
    }
    if (!current_status.ok()) {
        background_error_ = current_status;
        return current_status;
    }
    if (!close_status.ok()) return close_status;
    if (flushing_immutable) CrashPoint("flush_after_immutable_installation");
    // Phase 9: the old WAL, old MANIFEST, and any other recognized orphan are
    // unreferenced now. Cleanup never removes a file in version_.
    status = CleanupObsoleteFiles(directory_, version_);
    if (!status.ok()) {
        return status;
    }

    // The single maintenance job also drains compaction triggers. Foreground
    // writers may fill the active memtable while its merge work runs.
    return RunCompactions(lock);
}

void DBImpl::BackgroundLoop() {
    std::unique_lock lock(mutex_);
    while (true) {
        condition_.wait(lock, [&] { return stopping_ || ((immutable_ || maintenance_requested_) && background_error_.ok()); });
        if (stopping_) break;
        maintenance_busy_ = true;
        maintenance_requested_ = false;
        Status status = immutable_ ? FlushMemTable(lock) : RunCompactions(lock);
        maintenance_busy_ = false;
        if (!status.ok()) background_error_ = status;
        condition_.notify_all();
    }
}

void DBImpl::StopWorker() {
    { std::lock_guard lock(mutex_); stopping_ = true; condition_.notify_all(); }
    if (worker_.joinable()) worker_.join();
}

Status DBImpl::Drain(std::unique_lock<std::mutex>& lock) {
    if (!background_error_.ok()) return background_error_;
    if (!options_.background_maintenance) {
        return memtable_.empty() ? RunCompactions(lock) : FlushMemTable(lock);
    }
    condition_.wait(lock, [&] { return (!immutable_ && !maintenance_busy_ && !maintenance_requested_) || !background_error_.ok(); });
    if (!background_error_.ok()) return background_error_;
    if (!memtable_.empty()) {
        Status status = RotateMemTable();
        if (!status.ok()) return status;
        condition_.wait(lock, [&] { return (!immutable_ && !maintenance_busy_ && !maintenance_requested_) || !background_error_.ok(); });
    }
    return background_error_;
}

Status DBImpl::Close() {
    std::lock_guard writer(writer_mutex_);
    std::unique_lock lock(mutex_);
    if (closed_) return Status::Ok();
    Status status = Drain(lock);
    if (!status.ok() && !options_.background_maintenance) return status;
    stopping_ = true;
    condition_.notify_all();
    lock.unlock();
    if (worker_.joinable()) worker_.join();
    lock.lock();
    const Status close_status = wal_writer_->Close();
    closed_ = true;
    table_cache_.Clear();
    directory_lock_.reset();
    return status.ok() ? close_status : status;
}

void DBImpl::FailNextWalAppendForTesting() {
    std::lock_guard lock(mutex_); wal_writer_->FailNextAppendForTesting();
}
void DBImpl::FailNextWalSyncForTesting() {
    std::lock_guard lock(mutex_); wal_writer_->FailNextSyncForTesting();
}
void DBImpl::FailNextSstableBuildForTesting() {
    std::lock_guard lock(mutex_); fail_next_sstable_build_for_testing_ = true;
}
void DBImpl::FailNextNewWalCreationForTesting() {
    std::lock_guard lock(mutex_); fail_next_new_wal_creation_for_testing_ = true;
}
void DBImpl::FailNextManifestWriteForTesting() {
    std::lock_guard lock(mutex_); fail_next_manifest_write_for_testing_ = true;
}
void DBImpl::FailNextManifestRenameForTesting() {
    std::lock_guard lock(mutex_); fail_next_manifest_rename_for_testing_ = true;
}
void DBImpl::FailNextCurrentInstallForTesting() {
    std::lock_guard lock(mutex_); fail_next_current_install_for_testing_ = true;
}
void DBImpl::FailNextCurrentSyncForTesting() {
    std::lock_guard lock(mutex_); fail_next_current_sync_for_testing_ = true;
}
Status DBImpl::FlushForTesting() {
    std::lock_guard writer(writer_mutex_);
    std::unique_lock lock(mutex_);
    if (closed_) return Status::IOError("database is closed");
    return Drain(lock);
}

}  // namespace minilsm
