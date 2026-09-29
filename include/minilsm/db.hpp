#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "minilsm/iterator.hpp"
#include "minilsm/options.hpp"
#include "minilsm/status.hpp"
#include "minilsm/write_batch.hpp"

namespace minilsm {

class DBImpl;

// Counters are cumulative since Open or reset. The original SSTable/Bloom
// fields count Get; cache fields also count Scan; flush/stall fields count writes.
// Validation and compaction bypass read caches and are excluded.
struct ReadCounters {
    std::uint64_t sstable_files_considered = 0U;
    std::uint64_t bloom_filter_probes = 0U;
    std::uint64_t bloom_definite_negatives = 0U;
    std::uint64_t bloom_possible_positives = 0U;
    std::uint64_t sstable_data_block_reads = 0U;
    std::uint64_t block_cache_hits = 0U, block_cache_misses = 0U;
    std::uint64_t block_cache_evictions = 0U, physical_block_reads = 0U;
    std::uint64_t table_cache_hits = 0U, table_cache_misses = 0U;
    std::uint64_t write_stalls = 0U, flush_count = 0U;
};

// Cumulative work from successfully installed compactions since Open or the
// latest explicit reset. Bytes read are the exact selected input file sizes;
// bytes written and output count describe the installed replacement files.
struct CompactionCounters {
    std::uint64_t bytes_read = 0U;
    std::uint64_t bytes_written = 0U;
    std::uint64_t output_file_count = 0U, compaction_count = 0U;
};

class DB {
public:
    ~DB();

    static Status Open(
        const Options& options,
        const std::filesystem::path& directory,
        std::unique_ptr<DB>* db);

    Status Put(std::string_view key, std::string_view value);
    Status Delete(std::string_view key);
    Status Write(const WriteOptions& options, const WriteBatch& batch);
    [[nodiscard]] LookupResult Get(std::string_view key) const;
    [[nodiscard]] ReadCounters read_counters() const;
    void ResetReadCounters();
    [[nodiscard]] CompactionCounters compaction_counters() const;
    void ResetCompactionCounters();

    [[nodiscard]] std::unique_ptr<Iterator> Scan(
        std::optional<std::string> lower_bound,
        std::optional<std::string> upper_bound) const;

    Status Close();

private:
    explicit DB(std::unique_ptr<DBImpl> impl);

    std::unique_ptr<DBImpl> impl_;
};

}  // namespace minilsm
