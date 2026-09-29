#include "db/db_lifecycle.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <algorithm>

#include "common/file_io.hpp"
#include "common/file_names.hpp"
#include "manifest/manifest.hpp"
#include "minilsm/format_limits.hpp"

namespace minilsm {

DatabaseLock::~DatabaseLock() { if (descriptor_ >= 0) ::close(descriptor_); }

namespace {

Status PrepareDirectory(
    const Options& options,
    const std::filesystem::path& directory) {
    if (directory.empty()) {
        return Status::InvalidArgument(
            "database directory must not be empty");
    }

    std::error_code error;
    const bool exists = std::filesystem::exists(directory, error);
    if (error) {
        return Status::IOError(
            "check database directory '" + directory.string() +
            "': " + error.message());
    }
    if (exists) {
        if (!std::filesystem::is_directory(directory, error) || error) {
            return Status::IOError(
                "database path is not a directory: '" +
                directory.string() + "'");
        }
        return Status::Ok();
    }
    if (!options.create_if_missing) {
        return Status::NotFound(
            "database directory does not exist: '" +
            directory.string() + "'");
    }
    if (!std::filesystem::create_directories(directory, error) || error) {
        return Status::IOError(
            "create database directory '" + directory.string() +
            "': " + error.message());
    }

    const std::filesystem::path parent =
        directory.parent_path().empty()
            ? std::filesystem::path(".")
            : directory.parent_path();
    return SyncDirectory(parent);
}

Status ReadBoundedFile(
    const std::filesystem::path& path,
    std::size_t minimum_size,
    std::size_t maximum_size,
    std::string* bytes) {
    std::uint64_t file_size = 0U;
    Status status = GetFileSize(path, &file_size);
    if (!status.ok()) {
        return status;
    }
    if (file_size < minimum_size || file_size > maximum_size ||
        file_size > std::numeric_limits<std::size_t>::max()) {
        return Status::Corruption(
            "metadata file size is outside its version 1 bounds: '" +
            path.string() + "'");
    }

    std::unique_ptr<RandomAccessFileReader> reader;
    status = RandomAccessFileReader::Open(path, &reader);
    if (!status.ok()) {
        return status;
    }
    status = reader->Read(
        0U, static_cast<std::size_t>(file_size), bytes);
    if (!status.ok()) {
        return status;
    }
    return reader->Close();
}

Status RaiseNextFileNumberPastExistingNames(
    const std::vector<std::string>& names,
    Version* version) {
    for (const std::string& name : names) {
        ParsedFileName parsed;
        const Status status = ParseFileName(name, &parsed);
        if (!status.ok() || parsed.kind == FileKind::kCurrent) {
            continue;
        }
        if (parsed.file_number < version->next_file_number) {
            continue;
        }
        if (parsed.file_number ==
            std::numeric_limits<std::uint64_t>::max()) {
            return Status::IOError(
                "file-number space is exhausted by an existing file");
        }
        version->next_file_number = parsed.file_number + 1U;
    }
    return Status::Ok();
}

Status RecoverWal(
    const std::filesystem::path& directory,
    std::uint64_t wal_number,
    std::uint64_t installed_sequence,
    MemTable* memtable,
    std::uint64_t* replayed_last_sequence,
    std::unique_ptr<WalWriter>* wal_writer,
    bool sealed = false) {
    std::string wal_name;
    Status status = MakeWalFileName(
        wal_number, false, &wal_name);
    if (!status.ok()) {
        return status;
    }
    const std::filesystem::path wal_path = directory / wal_name;

    bool wal_exists = false;
    status = FileExists(wal_path, &wal_exists);
    if (!status.ok()) {
        return status;
    }
    if (!wal_exists) {
        return Status::Corruption(
            "MANIFEST references a missing active WAL '" +
            wal_name + "'");
    }

    std::unique_ptr<WalReader> reader;
    status = WalReader::Open(
        wal_path, wal_number, &reader);
    if (!status.ok()) {
        return status;
    }

    while (true) {
        WalRecord record;
        bool at_end = false;
        status = reader->ReadNext(&record, &at_end);
        if (!status.ok()) {
            return status;
        }
        if (at_end) {
            break;
        }
        if (record.internal_key.sequence_number <=
            std::max(installed_sequence, *replayed_last_sequence)) {
            return Status::Corruption(
                "active WAL replays a sequence already installed in SSTables");
        }
        *replayed_last_sequence = record.internal_key.sequence_number;
        status = memtable->Add(
            std::move(record.internal_key), std::move(record.value));
        if (!status.ok()) {
            return status;
        }
    }

    const std::uint64_t file_size = reader->file_size();
    const std::uint64_t valid_size = reader->valid_size();
    const bool has_torn_tail = reader->has_torn_tail();
    if (sealed && has_torn_tail) return Status::Corruption("sealed immutable WAL has a torn tail");
    status = reader->Close();
    if (!status.ok()) {
        return status;
    }

    status = WalWriter::OpenForAppend(
        wal_path, file_size, *replayed_last_sequence, wal_writer);
    if (!status.ok()) {
        return status;
    }
    if (has_torn_tail) {
        status = (*wal_writer)->TruncateTo(valid_size);
        if (!status.ok()) {
            return status;
        }
    }
    return Status::Ok();
}

Status InitializeNewDatabase(
    const std::filesystem::path& directory,
    RecoveredDatabaseState* state) {
    Version version;
    version.active_wal_file_number = 1U;
    version.manifest_file_number = 2U;
    version.next_file_number = 3U;

    std::string wal_name;
    Status status = MakeWalFileName(1U, false, &wal_name);
    if (!status.ok()) {
        return status;
    }
    std::unique_ptr<WalWriter> wal_writer;
    status = WalWriter::CreateNew(
        directory / wal_name, 1U, &wal_writer);
    if (!status.ok()) {
        return status;
    }
    status = SyncDirectory(directory);
    if (!status.ok()) {
        return status;
    }

    status = WriteManifestSnapshotFile(directory, version, false);
    if (!status.ok()) {
        return status;
    }
    bool current_replaced = false;
    status = InstallCurrentFile(
        directory,
        version.manifest_file_number,
        &current_replaced,
        false);
    if (!status.ok()) {
        return status;
    }

    state->version = std::move(version);
    state->wal_writer = std::move(wal_writer);
    state->next_sequence = 1U;
    return Status::Ok();
}

Status OpenExistingDatabase(
    const Options& options,
    const std::filesystem::path& directory,
    const std::vector<std::string>& names,
    RecoveredDatabaseState* state) {
    std::string current_bytes;
    Status status = ReadBoundedFile(
        directory / CurrentFileName(false), 30U, 30U, &current_bytes);
    if (!status.ok()) {
        return status;
    }
    std::uint64_t manifest_file_number = 0U;
    status = DecodeCurrent(current_bytes, &manifest_file_number);
    if (!status.ok()) {
        return status;
    }

    std::string manifest_name;
    status = MakeManifestFileName(
        manifest_file_number, false, &manifest_name);
    if (!status.ok()) {
        return status;
    }
    std::string manifest_bytes;
    bool manifest_exists = false;
    status = FileExists(
        directory / manifest_name, &manifest_exists);
    if (!status.ok()) {
        return status;
    }
    if (!manifest_exists) {
        return Status::Corruption(
            "CURRENT references a missing MANIFEST '" +
            manifest_name + "'");
    }
    status = ReadBoundedFile(
        directory / manifest_name,
        60U,
        kMaxManifestFileBytes,
        &manifest_bytes);
    if (!status.ok()) {
        return status;
    }

    Version version;
    status = DecodeManifestSnapshot(
        manifest_bytes, manifest_file_number, &version);
    if (!status.ok()) {
        return status;
    }
    status = RaiseNextFileNumberPastExistingNames(names, &version);
    if (!status.ok()) {
        return status;
    }

    for (std::vector<FileMetaData>& level : version.levels) {
        for (FileMetaData& metadata : level) {
            std::unique_ptr<SstableReader> reader;
            status = ValidateSstableFile(
                directory, &metadata, &reader, options.paranoid_checks);
            if (!status.ok()) {
                return status;
            }
        }
    }

    MemTable memtable;
    std::unique_ptr<MemTable> immutable;
    std::unique_ptr<WalWriter> wal_writer;
    std::uint64_t replayed_last_sequence = 0U;
    if (version.immutable_wal_file_number != 0U) {
        immutable = std::make_unique<MemTable>();
        std::unique_ptr<WalWriter> sealed;
        status = RecoverWal(directory, version.immutable_wal_file_number,
                            version.last_sequence, immutable.get(),
                            &replayed_last_sequence, &sealed, true);
        if (status.ok()) status = sealed->Close();
        if (!status.ok()) return status;
        if (immutable->empty()) return Status::Corruption("immutable WAL is empty");
    }
    status = RecoverWal(directory, version.active_wal_file_number,
                        version.last_sequence, &memtable,
                        &replayed_last_sequence, &wal_writer);
    if (!status.ok()) return status;

    // Resolve any earlier uncertain directory sync before deleting old authority.
    status = SyncDirectory(directory);
    if (!status.ok()) return status;
    status = CleanupObsoleteFiles(directory, version);
    if (!status.ok()) {
        return status;
    }

    const std::uint64_t last_sequence =
        replayed_last_sequence == 0U
            ? version.last_sequence
            : replayed_last_sequence;
    std::optional<std::uint64_t> next_sequence;
    if (last_sequence != std::numeric_limits<std::uint64_t>::max()) {
        next_sequence = last_sequence + 1U;
    }

    state->version = std::move(version);
    state->memtable = std::move(memtable);
    state->immutable = std::move(immutable);
    state->wal_writer = std::move(wal_writer);
    state->next_sequence = next_sequence;
    return Status::Ok();
}

}  // namespace

Status ValidateSstableFile(
    const std::filesystem::path& directory,
    FileMetaData* metadata,
    std::unique_ptr<SstableReader>* reader,
    bool paranoid) {
    if (metadata == nullptr || reader == nullptr) {
        return Status::InvalidArgument(
            "SSTable metadata validation received a null argument");
    }

    std::string name;
    Status status = MakeSstableFileName(
        metadata->file_number, false, &name);
    if (!status.ok()) {
        return status;
    }
    bool exists = false;
    status = FileExists(directory / name, &exists);
    if (!status.ok()) {
        return status;
    }
    if (!exists) {
        return Status::Corruption(
            "MANIFEST references a missing SSTable '" + name + "'");
    }
    std::unique_ptr<SstableReader> opened;
    status = SstableReader::Open(directory / name, &opened);
    if (!status.ok()) {
        return status;
    }

    if (!paranoid) {
        if (opened->file_size() != metadata->file_size ||
            opened->entry_count() != metadata->entry_count ||
            opened->index().empty() ||
            opened->index().back().largest_internal_key.user_key != metadata->largest_user_key) {
            return Status::Corruption("MANIFEST and SSTable metadata disagree");
        }
        metadata->largest_internal_key = opened->index().back().largest_internal_key;
        *reader = std::move(opened);
        return Status::Ok();
    }
    SstableProperties properties;
    status = opened->ValidateAndReadProperties(&properties);
    if (!status.ok()) {
        return status;
    }
    if (properties.file_size != metadata->file_size ||
        properties.entry_count != metadata->entry_count ||
        properties.smallest_sequence != metadata->smallest_sequence ||
        properties.largest_sequence != metadata->largest_sequence ||
        properties.smallest_internal_key.user_key !=
            metadata->smallest_user_key ||
        properties.largest_internal_key.user_key !=
            metadata->largest_user_key) {
        return Status::Corruption(
            "MANIFEST metadata does not match referenced SSTable '" +
            name + "'");
    }

    metadata->smallest_internal_key = properties.smallest_internal_key;
    metadata->largest_internal_key = properties.largest_internal_key;
    *reader = std::move(opened);
    return Status::Ok();
}

Status OpenOrCreateDatabaseState(
    const Options& options,
    const std::filesystem::path& directory,
    RecoveredDatabaseState* state) {
    if (state == nullptr) {
        return Status::InvalidArgument(
            "database-state open received a null output");
    }
    Status status = PrepareDirectory(options, directory);
    if (!status.ok()) {
        return status;
    }

    const int descriptor = ::open((directory / "LOCK").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (descriptor < 0) return Status::IOError("open LOCK: " + std::string(std::strerror(errno)));
    state->directory_lock = std::make_unique<DatabaseLock>(descriptor);
    if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        return Status::IOError("database directory is locked");
    }
    std::vector<std::string> names;
    status = ListDirectory(directory, &names);
    if (!status.ok()) {
        return status;
    }
    names.erase(std::remove(names.begin(), names.end(), "LOCK"), names.end());
    if (names.empty()) {
        if (!options.create_if_missing) {
            return Status::NotFound(
                "database directory contains no CairnKV database");
        }
        return InitializeNewDatabase(directory, state);
    }

    bool has_current = false;
    for (const std::string& name : names) {
        if (name == CurrentFileName(false)) {
            has_current = true;
            break;
        }
    }
    if (!has_current) {
        return Status::Corruption(
            "nonempty database directory is missing CURRENT");
    }
    return OpenExistingDatabase(options, directory, names, state);
}

}  // namespace minilsm
