#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

#include "common/file_names.hpp"
#include "db/db_impl.hpp"
#include "db/db_lifecycle.hpp"
#include "minilsm/db.hpp"
#include "test_harness.hpp"

namespace {
struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        std::string pattern = (std::filesystem::temp_directory_path() / "minilsm-revision-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        if (const char* made = ::mkdtemp(buffer.data())) path = made;
    }
    ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};

bool TestDirectoryLock() {
    TemporaryDirectory dir;
    minilsm::Options options;
    options.background_maintenance = false;
    std::unique_ptr<minilsm::DB> db, second;
    MINILSM_CHECK(minilsm::DB::Open(options, dir.path, &db).ok());
    MINILSM_CHECK(!minilsm::DB::Open(options, dir.path, &second).ok());
    const pid_t child = ::fork();
    MINILSM_CHECK(child >= 0);
    if (child == 0) {
        std::unique_ptr<minilsm::DB> attempted;
        ::_exit(minilsm::DB::Open(options, dir.path, &attempted).ok() ? 1 : 0);
    }
    int result = 0;
    MINILSM_CHECK(::waitpid(child, &result, 0) == child && WIFEXITED(result) && WEXITSTATUS(result) == 0);
    MINILSM_CHECK(db->Close().ok());
    MINILSM_CHECK(minilsm::DB::Open(options, dir.path, &second).ok());
    MINILSM_CHECK(second->Close().ok());
    return true;
}

bool TestScanOwnsMemoryAndTableInputs() {
    TemporaryDirectory dir;
    minilsm::Options options;
    options.background_maintenance = false;
    std::unique_ptr<minilsm::DBImpl> db;
    MINILSM_CHECK(minilsm::DBImpl::Open(options, dir.path, &db).ok());
    MINILSM_CHECK(db->Put("a", "one").ok());
    MINILSM_CHECK(db->FlushForTesting().ok());
    MINILSM_CHECK(db->Put("b", "two").ok());
    auto scan = db->Scan(std::nullopt, std::nullopt);
    MINILSM_CHECK(db->Delete("a").ok());
    MINILSM_CHECK(db->Put("b", "changed").ok());
    MINILSM_CHECK(db->FlushForTesting().ok());
    MINILSM_CHECK(db->Close().ok());
    db.reset();
    MINILSM_CHECK(scan->Valid() && scan->key() == "a" && scan->value() == "one");
    scan->Next();
    MINILSM_CHECK(scan->Valid() && scan->key() == "b" && scan->value() == "two");
    scan->Next();
    MINILSM_CHECK(!scan->Valid() && scan->status().ok());
    return true;
}

bool TestConcurrentBatchesAndBackgroundMaintenance() {
    TemporaryDirectory dir;
    minilsm::Options options;
    options.memtable_bytes_limit = 256U;
    options.sstable_block_target_bytes = 1024U;
    options.compaction_output_file_target_bytes = 1024U;
    options.l0_compaction_file_trigger = 2U;
    options.l1_max_bytes = 8192U;
    options.l2_max_bytes = 32768U;
    std::unique_ptr<minilsm::DBImpl> db;
    MINILSM_CHECK(minilsm::DBImpl::Open(options, dir.path, &db).ok());
    minilsm::WriteBatch initial;
    initial.Put("a", "0"); initial.Put("b", "0");
    MINILSM_CHECK(db->Write({}, initial).ok());
    std::atomic<bool> running{true}, good{true};
    std::vector<std::thread> readers;
    for (int thread = 0; thread < 3; ++thread) {
        readers.emplace_back([&] {
            while (running) {
                auto scan = db->Scan(std::nullopt, std::nullopt);
                if (!scan->Valid() || scan->key() != "a") { good = false; break; }
                const std::string value(scan->value());
                scan->Next();
                if (!scan->Valid() || scan->key() != "b" || scan->value() != value) { good = false; break; }
                scan->Next();
                if (scan->Valid() || !scan->status().ok() || !db->Get("a").status.ok()) { good = false; break; }
            }
        });
    }
    for (int value = 1; value <= 160 && good; ++value) {
        minilsm::WriteBatch batch;
        batch.Put("a", std::to_string(value)); batch.Put("b", std::to_string(value));
        if (!db->Write({}, batch).ok()) good = false;
    }
    running = false;
    for (auto& reader : readers) reader.join();
    MINILSM_CHECK(good);
    MINILSM_CHECK(db->FlushForTesting().ok());
    MINILSM_CHECK(db->read_counters().flush_count > 1U);
    MINILSM_CHECK(db->compaction_counters().compaction_count > 0U);
    MINILSM_CHECK(db->Close().ok());
    MINILSM_CHECK(minilsm::DBImpl::Open(options, dir.path, &db).ok());
    MINILSM_CHECK(db->Get("a").value == std::optional<std::string>("160"));
    MINILSM_CHECK(db->Get("b").value == std::optional<std::string>("160"));
    MINILSM_CHECK(db->Close().ok());
    return true;
}

bool TestStartupSchedulesCompactionWithoutNewWrites() {
    TemporaryDirectory dir;
    minilsm::Options options;
    options.background_maintenance = false;
    options.l0_compaction_file_trigger = 100U;
    std::unique_ptr<minilsm::DBImpl> db;
    MINILSM_CHECK(minilsm::DBImpl::Open(options, dir.path, &db).ok());
    for (int value = 0; value < 3; ++value) {
        MINILSM_CHECK(db->Put("same", std::to_string(value)).ok());
        MINILSM_CHECK(db->FlushForTesting().ok());
    }
    MINILSM_CHECK(db->Close().ok());
    db.reset();
    options.background_maintenance = true;
    options.l0_compaction_file_trigger = 2U;
    MINILSM_CHECK(minilsm::DBImpl::Open(options, dir.path, &db).ok());
    MINILSM_CHECK(db->FlushForTesting().ok());
    MINILSM_CHECK(db->compaction_counters().compaction_count > 0U);
    MINILSM_CHECK(db->Get("same").value == std::optional<std::string>("2"));
    MINILSM_CHECK(db->Close().ok());
    return true;
}

bool TestCacheAndAtomicBatchValidation() {
    TemporaryDirectory dir;
    minilsm::Options options;
    options.background_maintenance = false;
    std::unique_ptr<minilsm::DBImpl> db;
    MINILSM_CHECK(minilsm::DBImpl::Open(options, dir.path, &db).ok());
    minilsm::WriteBatch batch;
    batch.Put("key", "old"); batch.Delete("key"); batch.Put("key", "new");
    MINILSM_CHECK(db->Write({false}, batch).ok());
    MINILSM_CHECK(db->Get("key").value == std::optional<std::string>("new"));
    minilsm::WriteBatch invalid;
    invalid.Put("valid-prefix", "must-not-appear");
    invalid.Put(std::string(minilsm::kMaxUserKeyBytes + 1U, 'x'), "bad");
    MINILSM_CHECK(!db->Write({}, invalid).ok());
    MINILSM_CHECK(db->Get("valid-prefix").status.code() == minilsm::Status::Code::kNotFound);
    MINILSM_CHECK(db->FlushForTesting().ok());
    db->ResetReadCounters();
    MINILSM_CHECK(db->Get("key").status.ok());
    const auto first = db->read_counters();
    MINILSM_CHECK(db->Get("key").status.ok());
    const auto second = db->read_counters();
    MINILSM_CHECK(first.physical_block_reads == 1U);
    MINILSM_CHECK(second.physical_block_reads == first.physical_block_reads);
    MINILSM_CHECK(second.block_cache_hits > first.block_cache_hits);
    db->ResetReadCounters();
    MINILSM_CHECK(db->read_counters().physical_block_reads == 0U);
    MINILSM_CHECK(db->Close().ok());
    return true;
}

bool TestUncertainCurrentSyncLatchesError() {
    TemporaryDirectory dir;
    minilsm::Options options;
    options.background_maintenance = false;
    std::unique_ptr<minilsm::DBImpl> db;
    MINILSM_CHECK(minilsm::DBImpl::Open(options, dir.path, &db).ok());
    MINILSM_CHECK(db->Put("stable", "value").ok());
    db->FailNextCurrentSyncForTesting();
    MINILSM_CHECK(!db->FlushForTesting().ok());
    MINILSM_CHECK(db->Get("stable").value == std::optional<std::string>("value"));
    MINILSM_CHECK(!db->Put("blocked", "x").ok());
    MINILSM_CHECK(!db->FlushForTesting().ok());
    std::string old_wal;
    MINILSM_CHECK(minilsm::MakeWalFileName(1U, false, &old_wal).ok());
    MINILSM_CHECK(std::filesystem::exists(dir.path / old_wal));
    db.reset();
    MINILSM_CHECK(minilsm::DBImpl::Open(options, dir.path, &db).ok());
    MINILSM_CHECK(db->Get("stable").value == std::optional<std::string>("value"));
    MINILSM_CHECK(db->Close().ok());
    return true;
}

bool TestBackgroundFailureUnblocksDrainAndRecovers() {
    TemporaryDirectory dir;
    minilsm::Options options;
    options.memtable_bytes_limit = 1U;
    std::unique_ptr<minilsm::DBImpl> db;
    MINILSM_CHECK(minilsm::DBImpl::Open(options, dir.path, &db).ok());
    db->FailNextSstableBuildForTesting();
    MINILSM_CHECK(db->Put("durable", "value").ok());
    MINILSM_CHECK(!db->FlushForTesting().ok());
    MINILSM_CHECK(!db->Put("blocked", "value").ok());
    MINILSM_CHECK(db->Get("durable").value == std::optional<std::string>("value"));
    MINILSM_CHECK(!db->Close().ok());
    db.reset();
    MINILSM_CHECK(minilsm::DBImpl::Open(options, dir.path, &db).ok());
    MINILSM_CHECK(db->Get("durable").value == std::optional<std::string>("value"));
    MINILSM_CHECK(db->Close().ok());
    return true;
}

bool CreateTwoWalState(const std::filesystem::path& directory) {
    for (std::uint64_t number = 1U; number <= 2U; ++number) {
        std::string name;
        MINILSM_CHECK(minilsm::MakeWalFileName(number, false, &name).ok());
        std::unique_ptr<minilsm::WalWriter> writer;
        MINILSM_CHECK(minilsm::WalWriter::CreateNew(directory / name, number, &writer).ok());
        minilsm::WriteBatch batch;
        batch.Put(number == 1U ? "old" : "new", std::to_string(number));
        MINILSM_CHECK(writer->AppendBatch(number, batch, true).ok());
        MINILSM_CHECK(writer->Close().ok());
    }
    minilsm::Version version;
    version.active_wal_file_number = 2U;
    version.immutable_wal_file_number = 1U;
    version.manifest_file_number = 3U;
    version.next_file_number = 4U;
    MINILSM_CHECK(minilsm::WriteManifestSnapshotFile(directory, version, false).ok());
    bool replaced = false;
    MINILSM_CHECK(minilsm::InstallCurrentFile(directory, 3U, &replaced, false).ok());
    return true;
}

bool TestTwoWalRecoveryAndSealedTailCorruption() {
    for (bool torn : {false, true}) {
        TemporaryDirectory dir;
        MINILSM_CHECK(CreateTwoWalState(dir.path));
        if (torn) {
            std::string name;
            MINILSM_CHECK(minilsm::MakeWalFileName(1U, false, &name).ok());
            std::filesystem::resize_file(dir.path / name, std::filesystem::file_size(dir.path / name) - 1U);
        }
        std::unique_ptr<minilsm::DB> db;
        auto status = minilsm::DB::Open({}, dir.path, &db);
        if (torn) {
            MINILSM_CHECK(status.code() == minilsm::Status::Code::kCorruption);
        } else {
            MINILSM_CHECK(status.ok());
            MINILSM_CHECK(db->Get("old").value == std::optional<std::string>("1"));
            MINILSM_CHECK(db->Get("new").value == std::optional<std::string>("2"));
            MINILSM_CHECK(db->Close().ok());
        }
    }
    return true;
}
}  // namespace

int main() {
    MINILSM_RUN(TestDirectoryLock);
    MINILSM_RUN(TestScanOwnsMemoryAndTableInputs);
    MINILSM_RUN(TestConcurrentBatchesAndBackgroundMaintenance);
    MINILSM_RUN(TestCacheAndAtomicBatchValidation);
    MINILSM_RUN(TestStartupSchedulesCompactionWithoutNewWrites);
    MINILSM_RUN(TestUncertainCurrentSyncLatchesError);
    MINILSM_RUN(TestTwoWalRecoveryAndSealedTailCorruption);
    MINILSM_RUN(TestBackgroundFailureUnblocksDrainAndRecovers);
    return 0;
}
