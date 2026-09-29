#pragma once

#include <array>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

#include "cache/block_cache.hpp"
#include "cache/table_cache.hpp"
#include "db/db_lifecycle.hpp"
#include "manifest/version.hpp"
#include "memtable/memtable.hpp"
#include "minilsm/db.hpp"
#include "wal/wal.hpp"

namespace minilsm {
struct CompactionPlan;

class DBImpl {
public:
    ~DBImpl();
    static Status Open(const Options&, const std::filesystem::path&, std::unique_ptr<DBImpl>*);
    Status Put(std::string_view key, std::string_view value);
    Status Delete(std::string_view key);
    Status Write(const WriteOptions&, const WriteBatch&);
    [[nodiscard]] LookupResult Get(std::string_view key) const;
    [[nodiscard]] std::unique_ptr<Iterator> Scan(std::optional<std::string>, std::optional<std::string>) const;
    [[nodiscard]] ReadCounters read_counters() const;
    void ResetReadCounters();
    [[nodiscard]] CompactionCounters compaction_counters() const;
    void ResetCompactionCounters();
    Status Close();
    void FailNextWalAppendForTesting();
    void FailNextWalSyncForTesting();
    void FailNextSstableBuildForTesting();
    void FailNextNewWalCreationForTesting();
    void FailNextManifestWriteForTesting();
    void FailNextManifestRenameForTesting();
    void FailNextCurrentInstallForTesting();
    void FailNextCurrentSyncForTesting();
    void FailNextCompactionBeforeManifestForTesting();
    void FailNextCompactionCurrentInstallForTesting();
    Status FlushForTesting();
private:
    DBImpl(Options, std::filesystem::path, RecoveredDatabaseState);
    Status RotateMemTable(); // state mutex held; no maintenance job active
    Status FlushMemTable(std::unique_lock<std::mutex>&);
    Status RunCompactions(std::unique_lock<std::mutex>&);
    Status ExecuteCompaction(const CompactionPlan&, std::unique_lock<std::mutex>&);
    Status Drain(std::unique_lock<std::mutex>&);
    void BackgroundLoop();
    void StopWorker();
    Status LookupSstableFile(const std::shared_ptr<SstableReader>&, std::string_view, MemTableLookup*, ReadCounters*) const;

    // All state below is protected by mutex_, except immutable options/path.
    // writer_mutex_ serializes client writes and Close across condition waits.
    Options options_;
    std::filesystem::path directory_;
    mutable std::mutex mutex_;
    std::mutex writer_mutex_;
    std::condition_variable condition_;
    std::thread worker_;
    bool stopping_ = false, maintenance_busy_ = false;
    bool maintenance_requested_ = true; // check existing level pressure at Open
    Status background_error_;
    Version version_;
    MemTable memtable_;
    std::unique_ptr<MemTable> immutable_;
    std::unique_ptr<WalWriter> wal_writer_;
    std::unique_ptr<DatabaseLock> directory_lock_;
    std::optional<std::uint64_t> next_sequence_;
    std::shared_ptr<BlockCache> block_cache_;
    mutable TableCache table_cache_;
    mutable ReadCounters read_counters_;
    ReadCounters counter_base_;
    CompactionCounters compaction_counters_;
    std::array<std::string, kLevelCount> compaction_pointer_;
    bool fail_next_sstable_build_for_testing_ = false;
    bool fail_next_new_wal_creation_for_testing_ = false;
    bool fail_next_manifest_write_for_testing_ = false;
    bool fail_next_manifest_rename_for_testing_ = false;
    bool fail_next_current_install_for_testing_ = false;
    bool fail_next_current_sync_for_testing_ = false;
    bool fail_next_compaction_before_manifest_for_testing_ = false;
    bool fail_next_compaction_current_install_for_testing_ = false;
    bool closed_ = false;
};
}  // namespace minilsm
