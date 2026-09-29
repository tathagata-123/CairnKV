#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>

#include "manifest/version.hpp"
#include "memtable/memtable.hpp"
#include "minilsm/options.hpp"
#include "minilsm/status.hpp"
#include "sstable/sstable_reader.hpp"
#include "wal/wal.hpp"

namespace minilsm {

// POSIX directory ownership. The lock file is never unlinked.
class DatabaseLock {
public:
    explicit DatabaseLock(int descriptor) : descriptor_(descriptor) {}
    ~DatabaseLock();
    DatabaseLock(const DatabaseLock&) = delete;
    DatabaseLock& operator=(const DatabaseLock&) = delete;
private:
    int descriptor_;
};

struct RecoveredDatabaseState {
    Version version;
    MemTable memtable;
    std::unique_ptr<MemTable> immutable;
    std::unique_ptr<WalWriter> wal_writer;
    std::unique_ptr<DatabaseLock> directory_lock;
    std::optional<std::uint64_t> next_sequence;
};

Status OpenOrCreateDatabaseState(
    const Options& options,
    const std::filesystem::path& directory,
    RecoveredDatabaseState* state);

Status ValidateSstableFile(
    const std::filesystem::path& directory,
    FileMetaData* metadata,
    std::unique_ptr<SstableReader>* reader,
    bool paranoid = true);

Status WriteManifestSnapshotFile(
    const std::filesystem::path& directory,
    const Version& version,
    bool fail_before_rename_for_testing);

// current_replaced becomes true immediately after the atomic rename. A later
// directory-sync failure does not undo that visible commit point.
Status InstallCurrentFile(
    const std::filesystem::path& directory,
    std::uint64_t manifest_file_number,
    bool* current_replaced,
    bool fail_before_rename_for_testing,
    bool fail_after_rename_for_testing = false);

Status CleanupObsoleteFiles(
    const std::filesystem::path& directory,
    const Version& live_version);

}  // namespace minilsm
