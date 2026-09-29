#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>

#include "cache/table_cache.hpp"
#include "common/file_names.hpp"
#include "sstable/sstable_builder.hpp"
#include "test_harness.hpp"

namespace {
bool CacheLifetime() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("minilsm-table-cache-" + std::to_string(::getpid()));
    std::filesystem::create_directories(directory);
    for (std::uint64_t number = 1; number <= 3; ++number) {
        std::string name;
        MINILSM_CHECK(minilsm::MakeSstableFileName(number, false, &name).ok());
        std::unique_ptr<minilsm::SstableBuilder> builder;
        MINILSM_CHECK(minilsm::SstableBuilder::Create(
            directory / name, 4096, &builder).ok());
        MINILSM_CHECK(builder->Add({"key", number, minilsm::RecordType::kValue},
                                  "value").ok());
        MINILSM_CHECK(builder->Finish().ok());
    }
    minilsm::TableCache cache(directory, 1, nullptr);
    std::shared_ptr<minilsm::SstableReader> first, again, second;
    MINILSM_CHECK(cache.Get(1, &first).ok());
    MINILSM_CHECK(cache.Get(1, &again).ok());
    MINILSM_CHECK(first == again);
    std::weak_ptr<minilsm::SstableReader> old = first;
    MINILSM_CHECK(cache.Get(2, &second).ok());
    MINILSM_CHECK(cache.stats().hits == 1);
    MINILSM_CHECK(cache.stats().misses == 2);
    MINILSM_CHECK(cache.stats().evictions == 1);
    MINILSM_CHECK(cache.stats().resident_entries == 1);
    std::string name;
    MINILSM_CHECK(minilsm::MakeSstableFileName(1, false, &name).ok());
    MINILSM_CHECK(std::filesystem::remove(directory / name));
    // Eviction and unlink do not break an in-flight reader.
    const auto value = first->Get({"key", 1, minilsm::RecordType::kValue});
    MINILSM_CHECK(value.status.ok() && value.value == "value");
    first.reset();
    again.reset();
    MINILSM_CHECK(old.expired()); // Last owner releases the RAII file handle.
    MINILSM_CHECK(!cache.Get(1, &again).ok());
    cache.Erase(2);
    MINILSM_CHECK(cache.stats().resident_entries == 0);
    std::weak_ptr<minilsm::SstableReader> second_weak = second;
    second.reset();
    MINILSM_CHECK(second_weak.expired());
    minilsm::TableCache disabled(directory, 0, nullptr);
    MINILSM_CHECK(disabled.Get(3, &first).ok());
    MINILSM_CHECK(disabled.Get(3, &again).ok());
    MINILSM_CHECK(first != again);
    MINILSM_CHECK(disabled.stats().resident_entries == 0);
    cache.Clear();
    std::filesystem::remove_all(directory);
    return true;
}
}  // namespace

int main() { MINILSM_RUN(CacheLifetime); }
