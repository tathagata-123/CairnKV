#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>
#include "common/crash_point.hpp"
#include "db/db_impl.hpp"
#include "minilsm/db.hpp"
#include "test_harness.hpp"

namespace {
struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        std::string pattern = (std::filesystem::temp_directory_path() / "minilsm-async-crash-XXXXXX").string();
        std::vector<char> bytes(pattern.begin(), pattern.end());
        bytes.push_back('\0');
        if (const char* made = ::mkdtemp(bytes.data())) path = made;
    }
    ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};

bool TestRealAsyncAuthorityTransitions() {
    const char* boundaries[] = {
        "rotation_after_current_installation",
        "flush_after_sstable_temp_file_completion",
        "flush_after_sstable_final_rename",
        "flush_after_immutable_installation",
    };
    for (const char* boundary : boundaries) {
        TemporaryDirectory dir;
        minilsm::Options options;
        options.background_maintenance = false;
        std::unique_ptr<minilsm::DB> initial;
        MINILSM_CHECK(minilsm::DB::Open(options, dir.path, &initial).ok());
        MINILSM_CHECK(initial->Put("acknowledged", "before-child").ok());
        MINILSM_CHECK(initial->Close().ok());
        initial.reset(); // parent owns no background threads or database handles at fork
        const pid_t child = ::fork();
        MINILSM_CHECK(child >= 0);
        if (child == 0) {
            ::setenv("MINILSM_CRASH_AT", boundary, 1);
            options.background_maintenance = true;
            options.memtable_bytes_limit = 1U;
            std::unique_ptr<minilsm::DBImpl> db;
            if (!minilsm::DBImpl::Open(options, dir.path, &db).ok()) ::_exit(90);
            minilsm::WriteBatch batch;
            batch.Put("left", "complete");
            batch.Put("right", "complete");
            if (!db->Write({}, batch).ok()) ::_exit(91);
            if (!db->FlushForTesting().ok()) ::_exit(92);
            ::_exit(93); // every configured hook must be reached
        }
        int result = 0;
        MINILSM_CHECK(::waitpid(child, &result, 0) == child);
        MINILSM_CHECK(WIFEXITED(result) && WEXITSTATUS(result) == minilsm::kCrashPointExitCode);
        for (int reopen = 0; reopen < 2; ++reopen) {
            std::unique_ptr<minilsm::DB> db;
            options.background_maintenance = true;
            MINILSM_CHECK(minilsm::DB::Open(options, dir.path, &db).ok());
            MINILSM_CHECK(db->Get("acknowledged").value == std::optional<std::string>("before-child"));
            // Each crash boundary is after the complete batch was fsynced.
            MINILSM_CHECK(db->Get("left").value == std::optional<std::string>("complete"));
            MINILSM_CHECK(db->Get("right").value == std::optional<std::string>("complete"));
            MINILSM_CHECK(db->Close().ok());
        }
    }
    return true;
}
}  // namespace
int main() { MINILSM_RUN(TestRealAsyncAuthorityTransitions); return 0; }
