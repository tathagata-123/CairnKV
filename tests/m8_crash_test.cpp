#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>

#include "common/crash_point.hpp"
#include "common/file_io.hpp"
#include "common/file_names.hpp"
#include "db/db_impl.hpp"
#include "manifest/manifest.hpp"
#include "manifest/version.hpp"
#include "minilsm/db.hpp"
#include "minilsm/format_limits.hpp"
#include "minilsm/options.hpp"
#include "test_harness.hpp"
#include "legacy_options.hpp"

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern =
            (std::filesystem::temp_directory_path() /
             "minilsm-m8-crash-XXXXXX")
                .string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        if (created != nullptr) {
            path_ = created;
        }
    }

    ~TemporaryDirectory() {
        if (!path_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::filesystem::path g_executable;

bool ReadWholeFile(
    const std::filesystem::path& path,
    std::string* bytes) {
    std::uint64_t size = 0U;
    MINILSM_CHECK(minilsm::GetFileSize(path, &size).ok());
    std::unique_ptr<minilsm::RandomAccessFileReader> reader;
    MINILSM_CHECK(
        minilsm::RandomAccessFileReader::Open(path, &reader).ok());
    MINILSM_CHECK(
        reader->Read(0U, static_cast<std::size_t>(size), bytes).ok());
    MINILSM_CHECK(reader->Close().ok());
    return true;
}

bool ReadActiveVersion(
    const std::filesystem::path& directory,
    minilsm::Version* version) {
    std::string current;
    MINILSM_CHECK(ReadWholeFile(directory / "CURRENT", &current));
    std::uint64_t manifest_number = 0U;
    MINILSM_CHECK(
        minilsm::DecodeCurrent(current, &manifest_number).ok());
    std::string manifest_name;
    MINILSM_CHECK(minilsm::MakeManifestFileName(
        manifest_number, false, &manifest_name).ok());
    std::string manifest;
    MINILSM_CHECK(ReadWholeFile(directory / manifest_name, &manifest));
    MINILSM_CHECK(minilsm::DecodeManifestSnapshot(
        manifest, manifest_number, version).ok());
    MINILSM_CHECK(minilsm::ValidateVersionMetadata(*version).ok());
    return true;
}

minilsm::Options NoCompactionOptions() {
    minilsm::Options options = LegacyOptions();
    options.memtable_bytes_limit = 1024U * 1024U;
    options.l0_compaction_file_trigger =
        minilsm::kMaxManifestLiveFiles;
    options.l1_max_bytes = minilsm::kMaxDatabaseFileBytes;
    options.l2_max_bytes = minilsm::kMaxDatabaseFileBytes;
    return options;
}

bool InitializeEmptyDatabase(const std::filesystem::path& directory) {
    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(
        minilsm::DB::Open(NoCompactionOptions(), directory, &db).ok());
    MINILSM_CHECK(db->Close().ok());
    return true;
}

int RunChildOperation(
    std::string_view operation,
    const std::filesystem::path& directory) {
    if (operation == "write") {
        std::unique_ptr<minilsm::DBImpl> db;
        if (!minilsm::DBImpl::Open(
                 NoCompactionOptions(), directory, &db).ok()) {
            return 10;
        }
        return db->Put("new", "value").ok() ? 11 : 12;
    }
    if (operation == "flush") {
        std::unique_ptr<minilsm::DBImpl> db;
        if (!minilsm::DBImpl::Open(
                 NoCompactionOptions(), directory, &db).ok()) {
            return 13;
        }
        if (!db->Put("new", "value").ok()) {
            return 14;
        }
        return db->FlushForTesting().ok() ? 15 : 16;
    }
    if (operation == "compaction") {
        minilsm::Options options = NoCompactionOptions();
        options.l0_compaction_file_trigger = 2U;
        std::unique_ptr<minilsm::DB> db;
        if (!minilsm::DB::Open(options, directory, &db).ok()) {
            return 17;
        }
        return db->Close().ok() ? 18 : 19;
    }
    return 20;
}

bool LaunchCrashChild(
    std::string_view crash_point,
    std::string_view operation,
    const std::filesystem::path& directory) {
    const pid_t child = ::fork();
    MINILSM_CHECK(child >= 0);
    if (child == 0) {
        if (::setenv(
                "MINILSM_CRASH_AT",
                std::string(crash_point).c_str(),
                1) != 0) {
            std::_Exit(125);
        }
        const std::string executable = g_executable.string();
        const std::string point(crash_point);
        const std::string child_operation(operation);
        const std::string child_directory = directory.string();
        ::execl(
            executable.c_str(),
            executable.c_str(),
            "--child",
            point.c_str(),
            child_operation.c_str(),
            child_directory.c_str(),
            static_cast<char*>(nullptr));
        std::_Exit(126);
    }

    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    MINILSM_CHECK(WIFEXITED(status));
    MINILSM_CHECK(WEXITSTATUS(status) == minilsm::kCrashPointExitCode);
    return true;
}

bool ExpectOnlyNewValue(minilsm::DB* db) {
    const minilsm::LookupResult found = db->Get("new");
    MINILSM_CHECK(found.status.ok());
    MINILSM_CHECK(found.value == std::optional<std::string>("value"));
    std::unique_ptr<minilsm::Iterator> scan =
        db->Scan(std::nullopt, std::nullopt);
    MINILSM_CHECK(scan->Valid());
    MINILSM_CHECK(scan->key() == "new");
    MINILSM_CHECK(scan->value() == "value");
    scan->Next();
    MINILSM_CHECK(!scan->Valid());
    MINILSM_CHECK(scan->status().ok());
    return true;
}

bool TestWalCrashBeforeSyncAllowsEitherDurableState() {
    TemporaryDirectory directory;
    MINILSM_CHECK(InitializeEmptyDatabase(directory.path()));
    MINILSM_CHECK(LaunchCrashChild(
        "wal_after_write_before_sync", "write", directory.path()));

    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(minilsm::DB::Open(
        NoCompactionOptions(), directory.path(), &db).ok());
    const minilsm::LookupResult result = db->Get("new");
    MINILSM_CHECK(
        result.status.ok() ||
        result.status.code() == minilsm::Status::Code::kNotFound);
    std::unique_ptr<minilsm::Iterator> scan =
        db->Scan(std::nullopt, std::nullopt);
    if (result.status.ok()) {
        MINILSM_CHECK(result.value == std::optional<std::string>("value"));
        MINILSM_CHECK(scan->Valid());
        MINILSM_CHECK(scan->key() == "new");
        MINILSM_CHECK(scan->value() == "value");
        scan->Next();
    }
    MINILSM_CHECK(!scan->Valid());
    MINILSM_CHECK(scan->status().ok());
    minilsm::Version version;
    MINILSM_CHECK(ReadActiveVersion(directory.path(), &version));
    MINILSM_CHECK(version.manifest_file_number == 2U);
    MINILSM_CHECK(version.active_wal_file_number == 1U);
    MINILSM_CHECK(version.levels[0].empty());
    MINILSM_CHECK(db->Close().ok());
    return true;
}

bool TestWalCrashAfterSyncReplaysMutation() {
    TemporaryDirectory directory;
    MINILSM_CHECK(InitializeEmptyDatabase(directory.path()));
    MINILSM_CHECK(LaunchCrashChild(
        "wal_after_sync_before_memtable", "write", directory.path()));

    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(minilsm::DB::Open(
        NoCompactionOptions(), directory.path(), &db).ok());
    MINILSM_CHECK(ExpectOnlyNewValue(db.get()));
    minilsm::Version version;
    MINILSM_CHECK(ReadActiveVersion(directory.path(), &version));
    MINILSM_CHECK(version.manifest_file_number == 2U);
    MINILSM_CHECK(version.active_wal_file_number == 1U);
    MINILSM_CHECK(version.levels[0].empty());
    MINILSM_CHECK(db->Close().ok());
    return true;
}

bool CheckFlushCrash(
    std::string_view crash_point,
    bool current_was_replaced) {
    TemporaryDirectory directory;
    MINILSM_CHECK(InitializeEmptyDatabase(directory.path()));
    MINILSM_CHECK(
        LaunchCrashChild(crash_point, "flush", directory.path()));

    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(minilsm::DB::Open(
        NoCompactionOptions(), directory.path(), &db).ok());
    MINILSM_CHECK(ExpectOnlyNewValue(db.get()));
    minilsm::Version version;
    MINILSM_CHECK(ReadActiveVersion(directory.path(), &version));
    if (current_was_replaced) {
        MINILSM_CHECK(version.manifest_file_number == 5U);
        MINILSM_CHECK(version.active_wal_file_number == 4U);
        MINILSM_CHECK(version.levels[0].size() == 1U);
    } else {
        MINILSM_CHECK(version.manifest_file_number == 2U);
        MINILSM_CHECK(version.active_wal_file_number == 1U);
        MINILSM_CHECK(version.levels[0].empty());
    }
    MINILSM_CHECK(db->Close().ok());
    return true;
}

bool TestFlushCrashBoundaries() {
    MINILSM_CHECK(CheckFlushCrash(
        "flush_after_sstable_temp_file_completion", false));
    MINILSM_CHECK(CheckFlushCrash(
        "flush_after_sstable_final_rename", false));
    MINILSM_CHECK(CheckFlushCrash(
        "flush_after_new_wal_creation", false));
    MINILSM_CHECK(CheckFlushCrash(
        "manifest_after_rename_before_current_replacement", false));
    MINILSM_CHECK(CheckFlushCrash(
        "current_after_replacement_before_obsolete_file_deletion", true));
    return true;
}

bool PrepareTwoL0Files(const std::filesystem::path& directory) {
    std::unique_ptr<minilsm::DBImpl> db;
    MINILSM_CHECK(minilsm::DBImpl::Open(
        NoCompactionOptions(), directory, &db).ok());
    MINILSM_CHECK(db->Put("a", "one").ok());
    MINILSM_CHECK(db->FlushForTesting().ok());
    MINILSM_CHECK(db->Put("b", "two").ok());
    MINILSM_CHECK(db->FlushForTesting().ok());
    MINILSM_CHECK(db->Close().ok());
    minilsm::Version version;
    MINILSM_CHECK(ReadActiveVersion(directory, &version));
    MINILSM_CHECK(version.levels[0].size() == 2U);
    MINILSM_CHECK(version.levels[1].empty());
    return true;
}

bool ExpectCompactionMap(minilsm::DB* db) {
    MINILSM_CHECK(
        db->Get("a").value == std::optional<std::string>("one"));
    MINILSM_CHECK(
        db->Get("b").value == std::optional<std::string>("two"));
    std::unique_ptr<minilsm::Iterator> scan =
        db->Scan(std::nullopt, std::nullopt);
    MINILSM_CHECK(scan->Valid() && scan->key() == "a");
    scan->Next();
    MINILSM_CHECK(scan->Valid() && scan->key() == "b");
    scan->Next();
    MINILSM_CHECK(!scan->Valid() && scan->status().ok());
    return true;
}

bool CheckCompactionCrash(
    std::string_view crash_point,
    bool current_was_replaced) {
    TemporaryDirectory directory;
    MINILSM_CHECK(PrepareTwoL0Files(directory.path()));
    MINILSM_CHECK(
        LaunchCrashChild(crash_point, "compaction", directory.path()));

    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(minilsm::DB::Open(
        NoCompactionOptions(), directory.path(), &db).ok());
    MINILSM_CHECK(ExpectCompactionMap(db.get()));
    minilsm::Version version;
    MINILSM_CHECK(ReadActiveVersion(directory.path(), &version));
    if (current_was_replaced) {
        MINILSM_CHECK(version.levels[0].empty());
        MINILSM_CHECK(!version.levels[1].empty());
        MINILSM_CHECK(minilsm::ValidateLevelNonOverlap(
            version.levels[1], 1U).ok());
    } else {
        MINILSM_CHECK(version.levels[0].size() == 2U);
        MINILSM_CHECK(version.levels[1].empty());
    }
    MINILSM_CHECK(db->Close().ok());
    return true;
}

bool TestCompactionCrashBoundaries() {
    MINILSM_CHECK(CheckCompactionCrash(
        "compaction_after_outputs_before_manifest_installation", false));
    MINILSM_CHECK(CheckCompactionCrash(
        "current_after_replacement_before_obsolete_file_deletion", true));
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 5 && std::string_view(argv[1]) == "--child") {
        return RunChildOperation(argv[3], argv[4]);
    }
    if (argc != 1) {
        return 2;
    }
    ::unsetenv("MINILSM_CRASH_AT");
    g_executable = std::filesystem::absolute(argv[0]);
    MINILSM_RUN(TestWalCrashBeforeSyncAllowsEitherDurableState);
    MINILSM_RUN(TestWalCrashAfterSyncReplaysMutation);
    MINILSM_RUN(TestFlushCrashBoundaries);
    MINILSM_RUN(TestCompactionCrashBoundaries);
    return 0;
}
