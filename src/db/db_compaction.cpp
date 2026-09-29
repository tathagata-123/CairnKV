#include "db/db_impl.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/crash_point.hpp"
#include "common/file_io.hpp"
#include "common/file_names.hpp"
#include "common/internal_key.hpp"
#include "compaction/compaction.hpp"
#include "db/db_lifecycle.hpp"
#include "iterator/internal_iterator.hpp"
#include "iterator/merging_iterator.hpp"
#include "iterator/sstable_iterator_adapter.hpp"
#include "manifest/version.hpp"
#include "minilsm/format_limits.hpp"
#include "sstable/sstable_builder.hpp"
#include "sstable/sstable_reader.hpp"

namespace minilsm {

namespace {

struct PendingCompactionOutput {
    std::uint64_t file_number = 0U;
    std::string temporary_name;
    std::string final_name;
    FileMetaData metadata;
    std::unique_ptr<SstableReader> reader;
};

void SaturatingAdd(
    std::uint64_t amount,
    std::uint64_t* counter) {
    if (amount > std::numeric_limits<std::uint64_t>::max() - *counter) {
        *counter = std::numeric_limits<std::uint64_t>::max();
    } else {
        *counter += amount;
    }
}

Status AddInputFile(
    const std::filesystem::path& directory,
    const FileMetaData& file,
    std::vector<std::unique_ptr<InternalIterator>>* children,
    std::uint64_t* input_bytes) {
    if (file.file_size >
        std::numeric_limits<std::uint64_t>::max() - *input_bytes) {
        return Status::Corruption(
            "compaction input byte total overflows uint64");
    }
    *input_bytes += file.file_size;

    std::string name;
    Status status = MakeSstableFileName(file.file_number, false, &name);
    if (!status.ok()) {
        return status;
    }
    std::unique_ptr<SstableReader> reader;
    status = SstableReader::Open(directory / name, &reader);
    if (!status.ok()) {
        return status;
    }
    children->push_back(
        std::make_unique<SstableIteratorAdapter>(std::move(reader)));
    return Status::Ok();
}

Status StartOutputBuilder(
    const Options& options,
    const std::filesystem::path& directory,
    Version* next_version,
    std::uint64_t* file_number,
    std::string* temporary_name,
    std::string* final_name,
    std::unique_ptr<SstableBuilder>* builder) {
    Status status = AllocateFileNumber(next_version, file_number);
    if (!status.ok()) {
        return status;
    }
    status = MakeSstableFileName(*file_number, true, temporary_name);
    if (status.ok()) {
        status = MakeSstableFileName(*file_number, false, final_name);
    }
    if (!status.ok()) {
        return status;
    }
    bool final_exists = false;
    status = FileExists(directory / *final_name, &final_exists);
    if (!status.ok()) {
        return status;
    }
    if (final_exists) {
        return Status::IOError(
            "refusing to overwrite existing compaction output '" +
            *final_name + "'");
    }
    return SstableBuilder::Create(
        directory / *temporary_name,
        options.sstable_block_target_bytes,
        builder, options.prefix_compression);
}

Status FinishOutputBuilder(
    std::uint64_t file_number,
    std::string temporary_name,
    std::string final_name,
    std::unique_ptr<SstableBuilder>* builder,
    std::vector<PendingCompactionOutput>* outputs) {
    Status status = (*builder)->Finish();
    if (!status.ok()) {
        return status;
    }
    builder->reset();
    PendingCompactionOutput output;
    output.file_number = file_number;
    output.temporary_name = std::move(temporary_name);
    output.final_name = std::move(final_name);
    outputs->push_back(std::move(output));
    return Status::Ok();
}

Status MeasureLogicalEntry(
    const InternalKey& key,
    std::string_view value,
    std::uint64_t* size) {
    constexpr std::uint64_t kEntryFixedBytes = 8U;
    const std::uint64_t key_bytes =
        kMinInternalKeyBytes + key.user_key.size();
    if (key_bytes >
            std::numeric_limits<std::uint64_t>::max() - kEntryFixedBytes ||
        value.size() >
            std::numeric_limits<std::uint64_t>::max() -
                kEntryFixedBytes - key_bytes) {
        return Status::InvalidArgument(
            "compaction output entry size overflows uint64");
    }
    *size = kEntryFixedBytes + key_bytes + value.size();
    return Status::Ok();
}

void RemoveFilesByNumber(
    const std::set<std::uint64_t>& selected,
    std::vector<FileMetaData>* files) {
    std::vector<FileMetaData> retained;
    retained.reserve(files->size());
    for (FileMetaData& file : *files) {
        if (!selected.contains(file.file_number)) {
            retained.push_back(std::move(file));
        }
    }
    *files = std::move(retained);
}

FileMetaData MetadataFromProperties(
    std::uint64_t file_number,
    std::uint32_t level,
    const SstableProperties& properties) {
    FileMetaData metadata;
    metadata.file_number = file_number;
    metadata.level = level;
    metadata.file_size = properties.file_size;
    metadata.entry_count = properties.entry_count;
    metadata.smallest_sequence = properties.smallest_sequence;
    metadata.largest_sequence = properties.largest_sequence;
    metadata.smallest_user_key =
        properties.smallest_internal_key.user_key;
    metadata.largest_user_key =
        properties.largest_internal_key.user_key;
    metadata.smallest_internal_key = properties.smallest_internal_key;
    metadata.largest_internal_key = properties.largest_internal_key;
    return metadata;
}

Status BuildCompaction(const Options& options, const std::filesystem::path& directory,
                       const CompactionPlan& plan, std::uint64_t last_sequence,
                       Version* next_version, std::vector<PendingCompactionOutput>* outputs,
                       std::uint64_t* input_bytes, std::uint64_t* output_bytes) {
    std::vector<std::unique_ptr<InternalIterator>> children;
    children.reserve(
        plan.source_files.size() +
        plan.overlapping_destination_files.size());
    *input_bytes = 0U;
    Status status;
    for (const FileMetaData& file : plan.source_files) {
        status = AddInputFile(
            directory, file, &children, input_bytes);
        if (!status.ok()) {
            return status;
        }
    }
    for (const FileMetaData& file : plan.overlapping_destination_files) {
        status = AddInputFile(
            directory, file, &children, input_bytes);
        if (!status.ok()) {
            return status;
        }
    }

    MergingIterator merged(std::move(children));
    status = merged.SeekToFirst();
    if (!status.ok()) {
        return status;
    }

    std::unique_ptr<SstableBuilder> builder;
    std::uint64_t output_file_number = 0U;
    std::string temporary_name;
    std::string final_name;
    std::uint64_t current_logical_bytes = 0U;
    std::string current_last_user_key;

    while (merged.Valid()) {
        const InternalKey newest_key = merged.key();
        std::string newest_value;
        if (newest_key.record_type == RecordType::kValue) {
            newest_value.assign(merged.value());
        }

        // The merge is in authoritative full InternalKey order, so this first
        // record is newest. Consume every exact duplicate and older version.
        do {
            merged.Next();
            if (!merged.status().ok()) {
                return merged.status();
            }
        } while (merged.Valid() &&
                 merged.key().user_key == newest_key.user_key);

        const bool bottommost_safe =
            plan.source_level == 2U && plan.destination_level == 3U;
        const bool keep_for_sequence_high_water =
            newest_key.sequence_number == last_sequence;
        if (newest_key.record_type == RecordType::kTombstone &&
            bottommost_safe && !keep_for_sequence_high_water) {
            // The picker included every L3 file overlapping the complete L2
            // source range, and there is no deeper level. No older value for
            // this key can survive outside the inputs. The one conservative
            // exception preserves the frozen MANIFEST last-sequence invariant.
            continue;
        }

        std::uint64_t entry_bytes = 0U;
        status = MeasureLogicalEntry(
            newest_key, newest_value, &entry_bytes);
        if (!status.ok()) {
            return status;
        }

        // A boundary is considered only after an entire user-key version
        // group was consumed. Compaction currently keeps one record per group,
        // but this condition makes the boundary rule explicit and durable.
        if (builder != nullptr &&
            current_logical_bytes >=
                options.compaction_output_file_target_bytes &&
            current_last_user_key != newest_key.user_key) {
            status = FinishOutputBuilder(
                output_file_number,
                std::move(temporary_name),
                std::move(final_name),
                &builder,
                outputs);
            if (!status.ok()) {
                return status;
            }
            current_logical_bytes = 0U;
            current_last_user_key.clear();
        }
        if (builder == nullptr) {
            status = StartOutputBuilder(
                options,
                directory,
                next_version,
                &output_file_number,
                &temporary_name,
                &final_name,
                &builder);
            if (!status.ok()) {
                return status;
            }
        }
        status = builder->Add(newest_key, newest_value);
        if (!status.ok()) {
            return status;
        }
        if (entry_bytes >
            std::numeric_limits<std::uint64_t>::max() -
                current_logical_bytes) {
            return Status::InvalidArgument(
                "compaction output byte estimate overflows uint64");
        }
        current_logical_bytes += entry_bytes;
        current_last_user_key = newest_key.user_key;
    }
    if (!merged.status().ok()) {
        return merged.status();
    }
    if (builder != nullptr) {
        status = FinishOutputBuilder(
            output_file_number,
            std::move(temporary_name),
            std::move(final_name),
            &builder,
            outputs);
        if (!status.ok()) {
            return status;
        }
    }

    // Every temporary output is complete and fsynced at this point. Rename
    // the entire output set, then make all final directory entries durable.
    for (PendingCompactionOutput& output : *outputs) {
        status = AtomicRename(
            directory / output.temporary_name,
            directory / output.final_name);
        if (!status.ok()) {
            return status;
        }
    }
    if (!outputs->empty()) {
        status = SyncDirectory(directory);
        if (!status.ok()) {
            return status;
        }
    }

    // Reopen and fully validate each final output before it can be named by a
    // MANIFEST. This also verifies its Bloom filter against every stored key.
    *output_bytes = 0U;
    for (PendingCompactionOutput& output : *outputs) {
        status = SstableReader::Open(
            directory / output.final_name, &output.reader);
        if (!status.ok()) {
            return status;
        }
        SstableProperties properties;
        status = output.reader->ValidateAndReadProperties(&properties);
        if (!status.ok()) {
            return status;
        }
        output.metadata = MetadataFromProperties(
            output.file_number, plan.destination_level, properties);
        if (properties.file_size >
            std::numeric_limits<std::uint64_t>::max() - *output_bytes) {
            return Status::IOError(
                "compaction output byte total overflows uint64");
        }
        *output_bytes += properties.file_size;
    }

    return Status::Ok();
}

}  // namespace

Status DBImpl::RunCompactions(std::unique_lock<std::mutex>& lock) {
    while (true) {
        std::optional<CompactionPlan> plan;
        Status status = options_.legacy_compaction_for_testing
            ? PickCompaction(options_, version_, &plan)
            : PickCompaction(options_, version_, compaction_pointer_, &plan);
        if (!status.ok() || !plan.has_value()) {
            return status;
        }
        status = ExecuteCompaction(*plan, lock);
        if (!status.ok()) {
            return status;
        }
    }
}

Status DBImpl::ExecuteCompaction(const CompactionPlan& plan, std::unique_lock<std::mutex>& lock) {
    if (plan.destination_level != plan.source_level + 1U ||
        plan.source_level >= 3U || plan.source_files.empty()) {
        return Status::InvalidArgument("compaction plan is invalid");
    }

    // A previous failed attempt can leave only unreferenced outputs because
    // it did not replace CURRENT. Remove those before allocating fresh names.
    Status status = CleanupObsoleteFiles(directory_, version_);
    if (!status.ok()) {
        return status;
    }

    Version next_version = version_;
    std::vector<PendingCompactionOutput> outputs;
    std::uint64_t input_bytes = 0U, output_bytes = 0U;
    lock.unlock();
    status = BuildCompaction(options_, directory_, plan, next_version.last_sequence,
                             &next_version, &outputs, &input_bytes, &output_bytes);
    lock.lock();
    version_.next_file_number = next_version.next_file_number;
    if (!status.ok()) return status;

    CrashPoint("compaction_after_outputs_before_manifest_installation");

    if (fail_next_compaction_before_manifest_for_testing_) {
        fail_next_compaction_before_manifest_for_testing_ = false;
        return Status::IOError(
            "simulated failure after compaction outputs completed");
    }

    std::uint64_t manifest_file_number = 0U;
    status = AllocateFileNumber(&next_version, &manifest_file_number);
    if (!status.ok()) {
        return status;
    }
    version_.next_file_number = next_version.next_file_number;

    std::set<std::uint64_t> selected_numbers;
    for (const FileMetaData& file : plan.source_files) {
        selected_numbers.insert(file.file_number);
    }
    for (const FileMetaData& file : plan.overlapping_destination_files) {
        selected_numbers.insert(file.file_number);
    }
    RemoveFilesByNumber(
        selected_numbers, &next_version.levels[plan.source_level]);
    RemoveFilesByNumber(
        selected_numbers, &next_version.levels[plan.destination_level]);
    for (const PendingCompactionOutput& output : outputs) {
        next_version.levels[plan.destination_level].push_back(
            output.metadata);
    }
    SortLevelFilesByUserKey(
        &next_version.levels[plan.destination_level]);
    next_version.manifest_file_number = manifest_file_number;

    status = ValidateVersionMetadata(next_version);
    if (!status.ok()) {
        return status;
    }
    status = WriteManifestSnapshotFile(directory_, next_version, false);
    if (!status.ok()) {
        return status;
    }

    const bool fail_current =
        fail_next_compaction_current_install_for_testing_;
    fail_next_compaction_current_install_for_testing_ = false;
    const bool fail_sync = fail_next_current_sync_for_testing_;
    fail_next_current_sync_for_testing_ = false;
    bool current_replaced = false;
    const Status current_status = InstallCurrentFile(
        directory_,
        manifest_file_number,
        &current_replaced,
        fail_current, fail_sync);
    if (!current_replaced) {
        return current_status;
    }

    // CURRENT is the authority switch. Only now replace the in-memory Version
    // and reader set, account installed work, and make inputs eligible for
    // deletion. A following directory-sync or cleanup error cannot roll back.
    version_ = std::move(next_version);
    for (const std::uint64_t file_number : selected_numbers) {
        table_cache_.Erase(file_number);
        block_cache_->Erase(file_number);
    }
    compaction_pointer_[plan.source_level] = plan.source_largest_user_key;
    ++compaction_counters_.compaction_count;
    SaturatingAdd(input_bytes, &compaction_counters_.bytes_read);
    SaturatingAdd(output_bytes, &compaction_counters_.bytes_written);
    SaturatingAdd(
        static_cast<std::uint64_t>(outputs.size()),
        &compaction_counters_.output_file_count);

    if (!current_status.ok()) {
        background_error_ = current_status;
        return current_status;
    }
    return CleanupObsoleteFiles(directory_, version_);
}

CompactionCounters DBImpl::compaction_counters() const {
    std::lock_guard lock(mutex_);
    return compaction_counters_;
}

void DBImpl::ResetCompactionCounters() {
    std::lock_guard lock(mutex_);
    compaction_counters_ = CompactionCounters{};
}

void DBImpl::FailNextCompactionBeforeManifestForTesting() {
    std::lock_guard lock(mutex_);
    fail_next_compaction_before_manifest_for_testing_ = true;
}

void DBImpl::FailNextCompactionCurrentInstallForTesting() {
    std::lock_guard lock(mutex_);
    fail_next_compaction_current_install_for_testing_ = true;
}

}  // namespace minilsm
