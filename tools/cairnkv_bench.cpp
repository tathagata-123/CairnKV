#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "minilsm/db.hpp"
#include "minilsm/iterator.hpp"
#include "minilsm/options.hpp"
#include "minilsm/status.hpp"

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kKeyBytes = 24U;
constexpr std::size_t kValueBytes = 100U;
constexpr std::uint64_t kRandomPutSeed = 0x4D3842454E4348ULL;
constexpr std::uint64_t kExistingGetSeed = 0x4D38474554ULL;

struct BenchmarkScale {
    std::uint64_t sequential_puts = 20'000U;
    std::uint64_t random_puts = 20'000U;
    std::uint64_t random_key_space = 4'000U;
    std::uint64_t read_records = 20'000U;
    std::uint64_t get_queries = 100'000U;
    std::uint64_t short_scan_width = 100U;
    std::uint64_t short_scan_repetitions = 500U;
    std::uint64_t long_scan_width = 5'000U;
    std::uint64_t long_scan_repetitions = 20U;
    std::uint64_t compaction_puts = 10'000U;
    std::uint64_t compaction_key_space = 2'000U;
    std::vector<std::uint64_t> recovery_mutations = {
        100U, 1'000U, 10'000U};
};

struct FileStats {
    std::uint64_t wal_bytes = 0U;
    std::uint64_t sstable_bytes = 0U;
    std::uint64_t metadata_bytes = 0U;
    std::uint64_t other_bytes = 0U;
    std::uint64_t wal_files = 0U;
    std::uint64_t sstable_files = 0U;

    [[nodiscard]] std::uint64_t TotalBytes() const {
        return wal_bytes + sstable_bytes + metadata_bytes + other_bytes;
    }
};

class BenchmarkWorkspace {
public:
    BenchmarkWorkspace(
        const std::optional<std::filesystem::path>& requested_root,
        bool keep_data)
        : keep_data_(keep_data) {
        const std::filesystem::path base = requested_root.has_value()
            ? *requested_root
            : std::filesystem::temp_directory_path();
        std::error_code error;
        std::filesystem::create_directories(base, error);
        if (error) {
            throw std::runtime_error(
                "create benchmark root '" + base.string() +
                "': " + error.message());
        }

        const std::string prefix =
            "minilsm-bench-" + std::to_string(::getpid()) + "-" +
            std::to_string(
                Clock::now().time_since_epoch().count());
        for (std::uint32_t attempt = 0U; attempt < 100U; ++attempt) {
            const std::filesystem::path candidate =
                base / (prefix + "-" + std::to_string(attempt));
            error.clear();
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = candidate;
                return;
            }
            if (error &&
                error != std::errc::file_exists) {
                throw std::runtime_error(
                    "create benchmark workspace '" +
                    candidate.string() + "': " + error.message());
            }
        }
        throw std::runtime_error(
            "could not allocate a unique benchmark workspace");
    }

    ~BenchmarkWorkspace() {
        if (keep_data_ || path_.empty()) {
            return;
        }
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    BenchmarkWorkspace(const BenchmarkWorkspace&) = delete;
    BenchmarkWorkspace& operator=(const BenchmarkWorkspace&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
    bool keep_data_ = false;
};

[[nodiscard]] BenchmarkScale QuickScale() {
    BenchmarkScale scale;
    scale.sequential_puts = 1'000U;
    scale.random_puts = 1'000U;
    scale.random_key_space = 200U;
    scale.read_records = 1'000U;
    scale.get_queries = 5'000U;
    scale.short_scan_width = 20U;
    scale.short_scan_repetitions = 50U;
    scale.long_scan_width = 250U;
    scale.long_scan_repetitions = 4U;
    scale.compaction_puts = 1'000U;
    scale.compaction_key_space = 200U;
    scale.recovery_mutations = {50U, 250U, 1'000U};
    return scale;
}

[[nodiscard]] std::string FixedKey(
    char prefix,
    std::uint64_t number,
    std::size_t key_bytes = kKeyBytes) {
    const std::string digits = std::to_string(number);
    if (key_bytes < 2U || digits.size() > key_bytes - 1U) {
        throw std::runtime_error(
            "key width is too small for the benchmark key number");
    }
    std::string key(key_bytes, '0');
    key.front() = prefix;
    std::copy(digits.begin(), digits.end(), key.end() - digits.size());
    return key;
}

[[nodiscard]] std::string FixedValue(
    std::uint64_t variant,
    std::size_t value_bytes = kValueBytes) {
    if (value_bytes == 0U) {
        return {};
    }
    std::string value(
        value_bytes,
        static_cast<char>('a' + (variant % 26U)));
    const std::string digits = std::to_string(variant);
    const std::size_t copied = std::min(digits.size(), value.size());
    std::copy(
        digits.end() - copied, digits.end(), value.end() - copied);
    return value;
}

void RequireOk(const minilsm::Status& status, std::string_view action) {
    if (!status.ok()) {
        throw std::runtime_error(
            std::string(action) + ": " + status.message());
    }
}

[[nodiscard]] double ElapsedSeconds(Clock::duration elapsed) {
    return std::chrono::duration<double>(elapsed).count();
}

[[nodiscard]] double Rate(std::uint64_t count, double seconds) {
    return seconds == 0.0
        ? 0.0
        : static_cast<double>(count) / seconds;
}

[[nodiscard]] double Ratio(
    std::uint64_t numerator,
    std::uint64_t denominator) {
    return denominator == 0U
        ? 0.0
        : static_cast<double>(numerator) /
              static_cast<double>(denominator);
}

// A value WAL frame contains a four-byte payload size, a four-byte
// InternalKey size, the 13 fixed InternalKey bytes, the user key, a four-byte
// value size, the value, and a four-byte frame checksum. This excludes WAL
// file headers, whose count depends on flush transitions.
[[nodiscard]] std::uint64_t WalValueFrameBytes(
    std::size_t key_bytes,
    std::size_t value_bytes) {
    return 29U + key_bytes + value_bytes;
}

[[nodiscard]] FileStats MeasureFiles(
    const std::filesystem::path& directory) {
    FileStats stats;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(directory, error), end;
         iterator != end;
         iterator.increment(error)) {
        if (error) {
            throw std::runtime_error(
                "enumerate benchmark database '" + directory.string() +
                "': " + error.message());
        }
        if (!iterator->is_regular_file(error)) {
            if (error) {
                throw std::runtime_error(
                    "inspect benchmark file: " + error.message());
            }
            continue;
        }
        const std::uint64_t size = iterator->file_size(error);
        if (error) {
            throw std::runtime_error(
                "measure benchmark file: " + error.message());
        }
        const std::string name = iterator->path().filename().string();
        if (name.starts_with("WAL-") && name.ends_with(".log")) {
            stats.wal_bytes += size;
            ++stats.wal_files;
        } else if (
            name.starts_with("SST-") && name.ends_with(".sst")) {
            stats.sstable_bytes += size;
            ++stats.sstable_files;
        } else if (name == "CURRENT" ||
                   name.starts_with("MANIFEST-")) {
            stats.metadata_bytes += size;
        } else {
            stats.other_bytes += size;
        }
    }
    if (error) {
        throw std::runtime_error(
            "enumerate benchmark database '" + directory.string() +
            "': " + error.message());
    }
    return stats;
}

void PrintOptions(const minilsm::Options& options) {
    std::cout << "background_maintenance=" << options.background_maintenance << '\n'
              << "prefix_compression=" << options.prefix_compression << '\n'
              << "block_cache_bytes=" << options.block_cache_bytes << '\n'
              << "paranoid_checks=" << options.paranoid_checks << '\n';
    std::cout
        << "sstable_block_target_bytes="
        << options.sstable_block_target_bytes << '\n'
        << "memtable_bytes_limit=" << options.memtable_bytes_limit << '\n'
        << "l0_compaction_file_trigger="
        << options.l0_compaction_file_trigger << '\n'
        << "l1_max_bytes=" << options.l1_max_bytes << '\n'
        << "l2_max_bytes=" << options.l2_max_bytes << '\n'
        << "compaction_output_file_target_bytes="
        << options.compaction_output_file_target_bytes << '\n';
}

void PrintFileStats(const FileStats& stats) {
    std::cout
        << "final_wal_files=" << stats.wal_files << '\n'
        << "final_wal_bytes=" << stats.wal_bytes << '\n'
        << "final_sstable_files=" << stats.sstable_files << '\n'
        << "final_sstable_bytes=" << stats.sstable_bytes << '\n'
        << "final_metadata_bytes=" << stats.metadata_bytes << '\n'
        << "final_other_bytes=" << stats.other_bytes << '\n'
        << "final_storage_bytes=" << stats.TotalBytes() << '\n';
}

[[nodiscard]] std::string UtcTimestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    if (::gmtime_r(&now, &utc) == nullptr) {
        return "unavailable";
    }
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

[[nodiscard]] std::string OperatingSystem() {
#if defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#elif defined(__FreeBSD__)
    return "FreeBSD";
#else
    return "unknown";
#endif
}

[[nodiscard]] std::string Compiler() {
#if defined(__clang__)
    return std::string("Clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("GCC ") + __VERSION__;
#else
    return "unknown";
#endif
}

void PrintContext(
    bool quick,
    bool keep_data,
    const std::filesystem::path& workspace) {
    std::cout
        << std::fixed << std::setprecision(6)
        << "[context]\n"
        << "date_utc=" << UtcTimestamp() << '\n'
        << "os=" << OperatingSystem() << '\n'
        << "compiler=" << Compiler() << '\n'
#ifdef NDEBUG
        << "build_type=Release (NDEBUG defined)\n"
#else
        << "build_type=Debug-compatible (NDEBUG not defined)\n"
#endif
        << "hardware_concurrency="
        << std::thread::hardware_concurrency() << '\n'
        << "engine_execution=foreground writer and one background maintenance worker\n"
        << "durability=normal CairnKV WAL append and fsync before each "
           "memtable mutation\n"
        << "timing_clock=std::chrono::steady_clock\n"
        << "scale=" << (quick ? "quick" : "default") << '\n'
        << "workspace=" << workspace.string() << '\n'
        << "workspace_retained=" << (keep_data ? "true" : "false")
        << "\n\n";
}

void RunSequentialPut(
    const std::filesystem::path& directory,
    std::uint64_t operation_count) {
    const minilsm::Options options;
    const std::string value = FixedValue(0U);
    std::unique_ptr<minilsm::DB> db;
    RequireOk(
        minilsm::DB::Open(options, directory, &db),
        "open sequential Put database");
    db->ResetCompactionCounters();

    const Clock::time_point start = Clock::now();
    for (std::uint64_t index = 0U; index < operation_count; ++index) {
        RequireOk(
            db->Put(FixedKey('k', index), value),
            "sequential Put");
    }
    const double put_seconds = ElapsedSeconds(Clock::now() - start);
    const Clock::time_point close_start = Clock::now();
    RequireOk(db->Close(), "close sequential Put database");
    const double close_seconds =
        ElapsedSeconds(Clock::now() - close_start);

    const minilsm::CompactionCounters compaction =
        db->compaction_counters();
    const FileStats files = MeasureFiles(directory);
    const std::uint64_t user_bytes = operation_count *
        static_cast<std::uint64_t>(kKeyBytes + kValueBytes);
    const std::uint64_t wal_record_bytes = operation_count *
        WalValueFrameBytes(kKeyBytes, kValueBytes);

    std::cout
        << "[sequential_put]\n"
        << "workload=Put increasing fixed-width keys\n"
        << "operation_count=" << operation_count << '\n'
        << "key_bytes=" << kKeyBytes << '\n'
        << "value_bytes=" << kValueBytes << '\n'
        << "timed_region=Put calls; final Close is reported separately\n";
    PrintOptions(options);
    std::cout
        << "user_bytes_written=" << user_bytes << '\n'
        << "wal_record_bytes_written=" << wal_record_bytes << '\n'
        << "wal_record_bytes_definition=exact value-frame bytes; excludes "
           "WAL file headers\n"
        << "put_elapsed_seconds=" << put_seconds << '\n'
        << "operations_per_second="
        << Rate(operation_count, put_seconds) << '\n'
        << "user_bytes_per_second=" << Rate(user_bytes, put_seconds) << '\n'
        << "close_elapsed_seconds=" << close_seconds << '\n'
        << "compaction_bytes_read=" << compaction.bytes_read << '\n'
        << "compaction_bytes_written=" << compaction.bytes_written << '\n'
        << "compaction_output_file_count="
        << compaction.output_file_count << '\n';
    PrintFileStats(files);
    std::cout << '\n';
}

void RunRandomPut(
    const std::filesystem::path& directory,
    std::uint64_t operation_count,
    std::uint64_t key_space) {
    const minilsm::Options options;
    std::mt19937_64 random(kRandomPutSeed);
    std::uniform_int_distribution<std::uint64_t> distribution(
        0U, key_space - 1U);
    std::vector<std::uint64_t> key_numbers;
    key_numbers.reserve(operation_count);
    std::vector<bool> seen(key_space, false);
    for (std::uint64_t index = 0U; index < operation_count; ++index) {
        const std::uint64_t key_number = distribution(random);
        key_numbers.push_back(key_number);
        seen[key_number] = true;
    }
    std::vector<std::string> values;
    for (std::uint64_t variant = 0U; variant < 64U; ++variant) {
        values.push_back(FixedValue(variant));
    }

    std::unique_ptr<minilsm::DB> db;
    RequireOk(
        minilsm::DB::Open(options, directory, &db),
        "open random Put database");
    db->ResetCompactionCounters();
    const Clock::time_point start = Clock::now();
    for (std::uint64_t index = 0U; index < operation_count; ++index) {
        RequireOk(
            db->Put(
                FixedKey('k', key_numbers[index]),
                values[index % values.size()]),
            "random Put");
    }
    const double put_seconds = ElapsedSeconds(Clock::now() - start);
    const Clock::time_point close_start = Clock::now();
    RequireOk(db->Close(), "close random Put database");
    const double close_seconds =
        ElapsedSeconds(Clock::now() - close_start);

    std::uint64_t distinct_keys = 0U;
    for (const bool was_seen : seen) {
        if (was_seen) {
            ++distinct_keys;
        }
    }
    const std::uint64_t user_bytes = operation_count *
        static_cast<std::uint64_t>(kKeyBytes + kValueBytes);
    const std::uint64_t live_user_bytes = distinct_keys *
        static_cast<std::uint64_t>(kKeyBytes + kValueBytes);
    const std::uint64_t wal_record_bytes = operation_count *
        WalValueFrameBytes(kKeyBytes, kValueBytes);
    const minilsm::CompactionCounters compaction =
        db->compaction_counters();
    const FileStats files = MeasureFiles(directory);

    std::cout
        << "[random_put_overwrite]\n"
        << "workload=Put uniformly selected keys with overwrites\n"
        << "operation_count=" << operation_count << '\n'
        << "key_space=" << key_space << '\n'
        << "distinct_keys_written=" << distinct_keys << '\n'
        << "key_bytes=" << kKeyBytes << '\n'
        << "value_bytes=" << kValueBytes << '\n'
        << "seed=" << kRandomPutSeed << '\n'
        << "timed_region=Put calls; random choices are generated before "
           "timing; final Close is reported separately\n";
    PrintOptions(options);
    std::cout
        << "user_bytes_written=" << user_bytes << '\n'
        << "logical_live_user_bytes=" << live_user_bytes << '\n'
        << "wal_record_bytes_written=" << wal_record_bytes << '\n'
        << "wal_record_bytes_definition=exact value-frame bytes; excludes "
           "WAL file headers\n"
        << "put_elapsed_seconds=" << put_seconds << '\n'
        << "operations_per_second="
        << Rate(operation_count, put_seconds) << '\n'
        << "user_bytes_per_second=" << Rate(user_bytes, put_seconds) << '\n'
        << "close_elapsed_seconds=" << close_seconds << '\n'
        << "compaction_bytes_read=" << compaction.bytes_read << '\n'
        << "compaction_bytes_written=" << compaction.bytes_written << '\n'
        << "compaction_output_file_count="
        << compaction.output_file_count << '\n'
        << "compaction_write_amplification="
        << Ratio(compaction.bytes_written, user_bytes) << '\n'
        << "compaction_write_amplification_definition=compaction bytes "
           "written / submitted user key+value bytes; excludes WAL and "
           "flush SSTable bytes\n";
    PrintFileStats(files);
    std::cout << '\n';
}

[[nodiscard]] minilsm::Options ReadBenchmarkOptions() {
    minilsm::Options options;
    options.memtable_bytes_limit = 256U * 1024U;
    options.l0_compaction_file_trigger = 4U;
    options.l1_max_bytes = 1U * 1024U * 1024U;
    options.l2_max_bytes = 4U * 1024U * 1024U;
    options.compaction_output_file_target_bytes = 256U * 1024U;
    return options;
}

void RunScanCase(
    const minilsm::DB& db,
    std::string_view section,
    std::uint64_t record_count,
    std::uint64_t width,
    std::uint64_t repetitions) {
    std::uint64_t returned = 0U;
    std::uint64_t observed_bytes = 0U;
    const std::uint64_t start_positions = record_count - width + 1U;
    const Clock::time_point start = Clock::now();
    for (std::uint64_t scan = 0U; scan < repetitions; ++scan) {
        const std::uint64_t first = (scan * 131U) % start_positions;
        std::unique_ptr<minilsm::Iterator> iterator = db.Scan(
            FixedKey('k', 2U * first),
            FixedKey('k', 2U * (first + width)));
        std::uint64_t scan_count = 0U;
        while (iterator->Valid()) {
            observed_bytes += iterator->key().size();
            observed_bytes += iterator->value().size();
            ++scan_count;
            iterator->Next();
        }
        RequireOk(iterator->status(), "range Scan");
        if (scan_count != width) {
            throw std::runtime_error(
                "range Scan returned an unexpected number of keys");
        }
        returned += scan_count;
    }
    const double seconds = ElapsedSeconds(Clock::now() - start);

    std::cout
        << '[' << section << "]\n"
        << "workload=repeated public [lower, upper) Scan\n"
        << "database_keys=" << record_count << '\n'
        << "scan_count=" << repetitions << '\n'
        << "keys_per_scan=" << width << '\n'
        << "keys_returned=" << returned << '\n'
        << "observed_key_value_bytes=" << observed_bytes << '\n'
        << "elapsed_seconds=" << seconds << '\n'
        << "keys_per_second=" << Rate(returned, seconds) << "\n\n";
}

void RunReadAndScanBenchmarks(
    const std::filesystem::path& directory,
    const BenchmarkScale& scale) {
    const minilsm::Options options = ReadBenchmarkOptions();
    const std::string value = FixedValue(7U);
    std::unique_ptr<minilsm::DB> db;
    RequireOk(
        minilsm::DB::Open(options, directory, &db),
        "open read benchmark database for setup");
    for (std::uint64_t index = 0U;
         index < scale.read_records;
         ++index) {
        RequireOk(
            db->Put(FixedKey('k', 2U * index), value),
            "populate read benchmark database");
    }
    RequireOk(db->Close(), "close read benchmark setup database");
    db.reset();
    RequireOk(
        minilsm::DB::Open(options, directory, &db),
        "reopen read benchmark database");
    const FileStats files = MeasureFiles(directory);

    std::cout
        << "[read_scan_dataset]\n"
        << "setup_timing=included in no reported read or Scan timing\n"
        << "database_keys=" << scale.read_records << '\n'
        << "key_pattern=fixed-width even-numbered keys; odd-numbered keys "
           "are absent and interleaved\n"
        << "key_bytes=" << kKeyBytes << '\n'
        << "value_bytes=" << kValueBytes << '\n'
        << "logical_live_user_bytes="
        << scale.read_records *
               static_cast<std::uint64_t>(kKeyBytes + kValueBytes)
        << '\n';
    PrintOptions(options);
    PrintFileStats(files);
    std::cout << '\n';

    std::mt19937_64 random(kExistingGetSeed);
    std::uniform_int_distribution<std::uint64_t> existing_distribution(
        0U, scale.read_records - 1U);
    std::vector<std::uint64_t> existing_queries;
    existing_queries.reserve(scale.get_queries);
    for (std::uint64_t query = 0U;
         query < scale.get_queries;
         ++query) {
        existing_queries.push_back(existing_distribution(random));
    }

    db->ResetReadCounters();
    std::uint64_t value_bytes_observed = 0U;
    const Clock::time_point existing_start = Clock::now();
    for (const std::uint64_t index : existing_queries) {
        const minilsm::LookupResult result =
            db->Get(FixedKey('k', 2U * index));
        if (!result.status.ok() ||
            result.value != std::optional<std::string>(value)) {
            throw std::runtime_error(
                "existing-key Get returned an unexpected result");
        }
        value_bytes_observed += result.value->size();
    }
    const double existing_seconds =
        ElapsedSeconds(Clock::now() - existing_start);
    const minilsm::ReadCounters existing = db->read_counters();

    std::cout
        << "[existing_key_get]\n"
        << "workload=uniform existing-key Get after Close/reopen\n"
        << "query_count=" << scale.get_queries << '\n'
        << "seed=" << kExistingGetSeed << '\n'
        << "value_bytes_observed=" << value_bytes_observed << '\n'
        << "elapsed_seconds=" << existing_seconds << '\n'
        << "operations_per_second="
        << Rate(scale.get_queries, existing_seconds) << '\n'
        << "average_latency_microseconds="
        << (existing_seconds * 1'000'000.0 /
            static_cast<double>(scale.get_queries)) << '\n'
        << "sstable_files_considered="
        << existing.sstable_files_considered << '\n'
        << "bloom_filter_probes=" << existing.bloom_filter_probes << '\n'
        << "bloom_definite_negatives="
        << existing.bloom_definite_negatives << '\n'
        << "bloom_possible_positives="
        << existing.bloom_possible_positives << '\n'
        << "sstable_data_block_reads="
        << existing.sstable_data_block_reads << "\n\n";

    db->ResetReadCounters();
    const std::uint64_t missing_positions = scale.read_records - 1U;
    const Clock::time_point missing_start = Clock::now();
    for (std::uint64_t query = 0U;
         query < scale.get_queries;
         ++query) {
        const std::uint64_t position = query % missing_positions;
        const minilsm::LookupResult result =
            db->Get(FixedKey('k', 2U * position + 1U));
        if (result.status.code() !=
                minilsm::Status::Code::kNotFound ||
            result.value.has_value()) {
            throw std::runtime_error(
                "missing-key Get returned an unexpected result");
        }
    }
    const double missing_seconds =
        ElapsedSeconds(Clock::now() - missing_start);
    const minilsm::ReadCounters missing = db->read_counters();

    std::cout
        << "[missing_key_get]\n"
        << "workload=Get absent odd-numbered keys interleaved within the "
           "stored key range\n"
        << "query_count=" << scale.get_queries << '\n'
        << "elapsed_seconds=" << missing_seconds << '\n'
        << "operations_per_second="
        << Rate(scale.get_queries, missing_seconds) << '\n'
        << "average_latency_microseconds="
        << (missing_seconds * 1'000'000.0 /
            static_cast<double>(scale.get_queries)) << '\n'
        << "sstable_files_considered="
        << missing.sstable_files_considered << '\n'
        << "bloom_filter_probes=" << missing.bloom_filter_probes << '\n'
        << "bloom_definite_negatives="
        << missing.bloom_definite_negatives << '\n'
        << "bloom_possible_positives="
        << missing.bloom_possible_positives << '\n'
        << "bloom_definite_negative_rate="
        << Ratio(
               missing.bloom_definite_negatives,
               missing.bloom_filter_probes)
        << '\n'
        << "observed_bloom_false_positive_rate="
        << Ratio(
               missing.bloom_possible_positives,
               missing.bloom_filter_probes)
        << '\n'
        << "data_reads_avoided="
        << missing.bloom_definite_negatives << '\n'
        << "data_reads_avoided_definition=one candidate SSTable data "
           "lookup avoided per Bloom definite negative\n"
        << "sstable_data_block_reads="
        << missing.sstable_data_block_reads << "\n\n";

    RunScanCase(
        *db,
        "short_range_scan",
        scale.read_records,
        scale.short_scan_width,
        scale.short_scan_repetitions);
    RunScanCase(
        *db,
        "long_range_scan",
        scale.read_records,
        scale.long_scan_width,
        scale.long_scan_repetitions);
    RequireOk(db->Close(), "close read and Scan benchmark database");
}

[[nodiscard]] minilsm::Options CompactionStressOptions() {
    minilsm::Options options;
    options.sstable_block_target_bytes = 1U * 1024U;
    options.memtable_bytes_limit = 16U * 1024U;
    options.l0_compaction_file_trigger = 2U;
    options.l1_max_bytes = 64U * 1024U;
    options.l2_max_bytes = 256U * 1024U;
    options.compaction_output_file_target_bytes = 32U * 1024U;
    return options;
}

void RunCompactionStress(
    const std::filesystem::path& directory,
    std::uint64_t operation_count,
    std::uint64_t key_space) {
    const minilsm::Options options = CompactionStressOptions();
    constexpr std::uint64_t seed = 0x4D38434F4D504143ULL;
    std::mt19937_64 random(seed);
    std::uniform_int_distribution<std::uint64_t> distribution(
        0U, key_space - 1U);
    std::vector<std::uint64_t> key_numbers;
    key_numbers.reserve(operation_count);
    std::vector<bool> seen(key_space, false);
    for (std::uint64_t index = 0U; index < operation_count; ++index) {
        const std::uint64_t number = distribution(random);
        key_numbers.push_back(number);
        seen[number] = true;
    }
    std::vector<std::string> values;
    for (std::uint64_t variant = 0U; variant < 64U; ++variant) {
        values.push_back(FixedValue(variant));
    }

    std::unique_ptr<minilsm::DB> db;
    RequireOk(
        minilsm::DB::Open(options, directory, &db),
        "open compaction-stress database");
    db->ResetCompactionCounters();
    const Clock::time_point start = Clock::now();
    for (std::uint64_t index = 0U; index < operation_count; ++index) {
        RequireOk(
            db->Put(
                FixedKey('k', key_numbers[index]),
                values[index % values.size()]),
            "compaction-stress Put");
    }
    const double put_seconds = ElapsedSeconds(Clock::now() - start);
    const Clock::time_point close_start = Clock::now();
    RequireOk(db->Close(), "close compaction-stress database");
    const double close_seconds =
        ElapsedSeconds(Clock::now() - close_start);

    std::uint64_t distinct_keys = 0U;
    for (const bool was_seen : seen) {
        if (was_seen) {
            ++distinct_keys;
        }
    }
    const std::uint64_t submitted_user_bytes = operation_count *
        static_cast<std::uint64_t>(kKeyBytes + kValueBytes);
    const std::uint64_t logical_live_user_bytes = distinct_keys *
        static_cast<std::uint64_t>(kKeyBytes + kValueBytes);
    const minilsm::CompactionCounters compaction =
        db->compaction_counters();
    const FileStats files = MeasureFiles(directory);

    std::cout
        << "[compaction_stress]\n"
        << "workload=random Put/overwrite with tiny thresholds and "
           "background compaction\n"
        << "operation_count=" << operation_count << '\n'
        << "key_space=" << key_space << '\n'
        << "distinct_live_keys=" << distinct_keys << '\n'
        << "key_bytes=" << kKeyBytes << '\n'
        << "value_bytes=" << kValueBytes << '\n'
        << "seed=" << seed << '\n'
        << "timed_region=Put calls; random choices are generated before "
           "timing; final Close is reported separately\n";
    PrintOptions(options);
    std::cout
        << "submitted_user_bytes=" << submitted_user_bytes << '\n'
        << "logical_live_user_bytes=" << logical_live_user_bytes << '\n'
        << "put_elapsed_seconds=" << put_seconds << '\n'
        << "operations_per_second="
        << Rate(operation_count, put_seconds) << '\n'
        << "close_elapsed_seconds=" << close_seconds << '\n'
        << "compaction_bytes_read=" << compaction.bytes_read << '\n'
        << "compaction_bytes_written=" << compaction.bytes_written << '\n'
        << "compaction_output_file_count="
        << compaction.output_file_count << '\n'
        << "compaction_write_amplification="
        << Ratio(compaction.bytes_written, submitted_user_bytes) << '\n'
        << "compaction_write_amplification_definition=compaction bytes "
           "written / submitted user key+value bytes; excludes WAL and "
           "flush SSTable bytes\n"
        << "live_sstable_space_amplification="
        << Ratio(files.sstable_bytes, logical_live_user_bytes) << '\n'
        << "live_sstable_space_amplification_definition=final live SSTable "
           "bytes / logical live user key+value bytes\n";
    PrintFileStats(files);
    std::cout << '\n';
}

[[nodiscard]] minilsm::Options RecoveryOptions() {
    minilsm::Options options;
    options.memtable_bytes_limit = 64U * 1024U * 1024U;
    return options;
}

[[noreturn]] void WriteRecoveryWalInChild(
    const std::filesystem::path& directory,
    std::uint64_t mutation_count) {
    const minilsm::Options options = RecoveryOptions();
    const std::string value = FixedValue(11U);
    std::unique_ptr<minilsm::DB> db;
    minilsm::Status status = minilsm::DB::Open(options, directory, &db);
    if (!status.ok()) {
        std::cerr << "recovery setup Open failed: "
                  << status.message() << '\n';
        std::cerr.flush();
        std::_Exit(2);
    }
    for (std::uint64_t index = 0U; index < mutation_count; ++index) {
        status = db->Put(FixedKey('k', index), value);
        if (!status.ok()) {
            std::cerr << "recovery setup Put failed: "
                      << status.message() << '\n';
            std::cerr.flush();
            std::_Exit(3);
        }
    }

    // Every accepted Put has already fsynced its WAL record. _Exit deliberately
    // skips DB destruction and Close, leaving all mutations for WAL replay.
    std::_Exit(0);
}

void RunRecoveryCase(
    const std::filesystem::path& directory,
    std::uint64_t mutation_count) {
    std::cout.flush();
    const pid_t child = ::fork();
    if (child < 0) {
        throw std::runtime_error("fork recovery setup child failed");
    }
    if (child == 0) {
        WriteRecoveryWalInChild(directory, mutation_count);
    }

    int child_status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(child, &child_status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child || !WIFEXITED(child_status) ||
        WEXITSTATUS(child_status) != 0) {
        throw std::runtime_error(
            "recovery setup child did not exit successfully");
    }

    const FileStats before_recovery = MeasureFiles(directory);
    if (before_recovery.wal_files != 1U ||
        before_recovery.sstable_files != 0U) {
        throw std::runtime_error(
            "recovery setup unexpectedly flushed the active WAL");
    }

    const minilsm::Options options = RecoveryOptions();
    std::unique_ptr<minilsm::DB> db;
    const Clock::time_point start = Clock::now();
    RequireOk(
        minilsm::DB::Open(options, directory, &db),
        "open database for measured WAL recovery");
    const double recovery_seconds =
        ElapsedSeconds(Clock::now() - start);

    const minilsm::LookupResult recovered =
        db->Get(FixedKey('k', mutation_count - 1U));
    if (!recovered.status.ok() ||
        recovered.value != std::optional<std::string>(FixedValue(11U))) {
        throw std::runtime_error(
            "WAL recovery did not expose the final mutation");
    }

    std::cout
        << "[recovery_wal_" << mutation_count << "]\n"
        << "workload=Open after child _Exit with an unflushed synced WAL\n"
        << "wal_mutation_count=" << mutation_count << '\n'
        << "key_bytes=" << kKeyBytes << '\n'
        << "value_bytes=" << kValueBytes << '\n'
        << "wal_bytes=" << before_recovery.wal_bytes << '\n'
        << "recovery_elapsed_seconds=" << recovery_seconds << '\n'
        << "recovered_mutations_per_second="
        << Rate(mutation_count, recovery_seconds) << '\n'
        << "timed_region=DB::Open including metadata/SSTable validation and "
           "active WAL replay; verification and Close excluded\n";
    PrintOptions(options);
    std::cout << '\n';

    RequireOk(db->Close(), "close recovered database");
}

void RunRecoveryBenchmarks(
    const std::filesystem::path& workspace,
    const std::vector<std::uint64_t>& mutation_counts) {
    for (const std::uint64_t mutation_count : mutation_counts) {
        RunRecoveryCase(
            workspace /
                ("recovery-" + std::to_string(mutation_count)),
            mutation_count);
    }
}

void PrintUsage(std::string_view executable) {
    std::cout
        << "Usage: " << executable
        << " [--quick] [--keep-data] [--root DIRECTORY]\n\n"
        << "Runs the CairnKV benchmark suite. --quick uses smaller workloads "
           "for tool validation. --root selects the parent of a newly "
           "created benchmark workspace. Data is removed unless "
           "--keep-data is supplied.\n";
}

}  // namespace

int main(int argc, char** argv) {
    bool quick = false;
    bool keep_data = false;
    std::optional<std::filesystem::path> requested_root;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--quick") {
            quick = true;
        } else if (argument == "--keep-data") {
            keep_data = true;
        } else if (argument == "--root") {
            if (index + 1 >= argc) {
                std::cerr << "--root requires a directory\n";
                return 2;
            }
            requested_root = std::filesystem::path(argv[++index]);
        } else if (argument == "--help" || argument == "-h") {
            PrintUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            PrintUsage(argv[0]);
            return 2;
        }
    }

    try {
        const BenchmarkScale scale = quick
            ? QuickScale()
            : BenchmarkScale{};
        BenchmarkWorkspace workspace(requested_root, keep_data);
        PrintContext(quick, keep_data, workspace.path());

        RunSequentialPut(
            workspace.path() / "sequential-put",
            scale.sequential_puts);
        RunRandomPut(
            workspace.path() / "random-put",
            scale.random_puts,
            scale.random_key_space);
        RunReadAndScanBenchmarks(
            workspace.path() / "read-scan",
            scale);
        RunCompactionStress(
            workspace.path() / "compaction-stress",
            scale.compaction_puts,
            scale.compaction_key_space);
        RunRecoveryBenchmarks(workspace.path(), scale.recovery_mutations);
    } catch (const std::exception& error) {
        std::cerr << "CairnKV benchmark failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
