#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/utsname.h>
#include <unistd.h>

#include "minilsm/db.hpp"
#ifndef MINILSM_LEGACY
#include "minilsm/write_batch.hpp"
#endif

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::uint64_t kSeed = 20260929U;
constexpr std::size_t kValueBytes = 100U;

void Check(const minilsm::Status& status) {
    if (!status.ok()) throw std::runtime_error(status.message());
}

double Seconds(Clock::duration elapsed) {
    return std::chrono::duration<double>(elapsed).count();
}

std::string Key(std::size_t number) {
    char key[64];
    std::snprintf(key, sizeof(key), "customer:region-east:%012zu", number);
    return key;
}

std::string Value(std::size_t number) {
    std::string value(kValueBytes, 'x');
    for (std::size_t i = 0U; i < value.size(); ++i) value[i] = static_cast<char>('a' + (number + i) % 26U);
    return value;
}

class Workspace {
public:
    Workspace() {
        std::string pattern = (std::filesystem::temp_directory_path() / "minilsm-experiments-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* result = ::mkdtemp(writable.data());
        if (result == nullptr) throw std::runtime_error("mkdtemp failed");
        path = result;
    }
    ~Workspace() { std::error_code error; std::filesystem::remove_all(path, error); }
    std::filesystem::path path;
};

std::uint64_t TableBytes(const std::filesystem::path& path) {
    std::uint64_t bytes = 0U;
    for (const auto& file : std::filesystem::directory_iterator(path)) {
        if (file.path().extension() == ".sst") bytes += file.file_size();
    }
    return bytes;
}

// The timed samples are individual public API calls. Batch latency describes
// one whole batch; throughput counts mutations, so these units are explicit.
void Latencies(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&](std::size_t numerator) {
        return samples[std::min(samples.size() - 1U, (samples.size() * numerator + 99U) / 100U - 1U)];
    };
    std::cout << ",\"p50_call_us\":" << percentile(50U)
              << ",\"p95_call_us\":" << percentile(95U)
              << ",\"p99_call_us\":" << percentile(99U);
}

minilsm::Options WriteOptions(bool background) {
    minilsm::Options options;
    options.memtable_bytes_limit = 16U * 1024U;
    options.l0_compaction_file_trigger = 4U;
    options.l1_max_bytes = 64U * 1024U;
    options.l2_max_bytes = 256U * 1024U;
    options.compaction_output_file_target_bytes = 32U * 1024U;
#ifndef MINILSM_LEGACY
    options.background_maintenance = background;
    options.prefix_compression = false;
    options.block_cache_bytes = 0U;
    options.paranoid_checks = true;
#else
    (void)background;
#endif
    return options;
}

void Maintenance(const std::filesystem::path& path, std::size_t count, bool background) {
    std::mt19937_64 random(kSeed);
    const std::size_t key_space = count / 3U;
    std::vector<std::string> keys, values;
    for (std::size_t i = 0U; i < count; ++i) {
        keys.push_back(Key(random() % key_space));
        values.push_back(Value(i));
    }
    std::unique_ptr<minilsm::DB> db;
    Check(minilsm::DB::Open(WriteOptions(background), path, &db));
    std::vector<double> samples;
    samples.reserve(count);
    const auto begin = Clock::now();
    for (std::size_t i = 0U; i < count; ++i) {
        const auto call = Clock::now();
        Check(db->Put(keys[i], values[i]));
        samples.push_back(Seconds(Clock::now() - call) * 1e6);
    }
    const double foreground = Seconds(Clock::now() - begin);
    const auto close = Clock::now();
    Check(db->Close());
    const double drain = Seconds(Clock::now() - close);
    const auto counters = db->compaction_counters();
    const auto table_bytes = TableBytes(path);
    std::cout << "{\"experiment\":\"maintenance\",\"engine\":\""
#ifdef MINILSM_LEGACY
              << "original"
#else
              << "cairnkv"
#endif
              << "\",\"background\":" << (background ? "true" : "false")
              << ",\"sync\":true,\"mutations\":" << count
              << ",\"foreground_seconds\":" << foreground
              << ",\"close_drain_seconds\":" << drain
              << ",\"foreground_ops_per_second\":" << count / foreground
              << ",\"including_drain_ops_per_second\":" << count / (foreground + drain)
              << ",\"compaction_bytes_read\":" << counters.bytes_read
              << ",\"compaction_bytes_written\":" << counters.bytes_written
              << ",\"compaction_only_write_amplification\":"
              << static_cast<double>(counters.bytes_written) / (count * (Key(0).size() + kValueBytes))
              << ",\"sstable_bytes\":" << table_bytes;
    Latencies(samples);
    std::cout << "}\n";
    // Verify all final overwrites after recovery, outside the timed region.
    Check(minilsm::DB::Open(WriteOptions(background), path, &db));
    std::vector<std::string> expected(key_space);
    std::mt19937_64 verifier(kSeed);
    for (std::size_t i = 0U; i < count; ++i) expected[verifier() % key_space] = values[i];
    for (std::size_t i = 0U; i < key_space; ++i) {
        const auto result = db->Get(Key(i));
        if (!expected[i].empty() && (!result.status.ok() || result.value != expected[i])) {
            throw std::runtime_error("maintenance recovery mismatch");
        }
    }
    Check(db->Close());
}

#ifndef MINILSM_LEGACY
void Batching(const std::filesystem::path& root, std::size_t count) {
    for (const std::size_t size : {1U, 10U, 100U, 1000U}) {
        minilsm::Options options;
        options.memtable_bytes_limit = 64U * 1024U * 1024U;
        std::unique_ptr<minilsm::DB> db;
        Check(minilsm::DB::Open(options, root / ("batch-" + std::to_string(size)), &db));
        std::vector<minilsm::WriteBatch> batches;
        for (std::size_t offset = 0U; offset < count; offset += size) {
            minilsm::WriteBatch batch;
            for (std::size_t i = offset; i < std::min(offset + size, count); ++i) batch.Put(Key(i), Value(i));
            batches.push_back(std::move(batch));
        }
        std::vector<double> samples;
        samples.reserve(batches.size());
        const auto begin = Clock::now();
        for (const auto& batch : batches) {
            const auto call = Clock::now();
            Check(db->Write(minilsm::WriteOptions{true}, batch));
            samples.push_back(Seconds(Clock::now() - call) * 1e6);
        }
        const double elapsed = Seconds(Clock::now() - begin);
        std::cout << "{\"experiment\":\"batching\",\"sync\":true,\"batch_size\":" << size
                  << ",\"mutations\":" << count << ",\"wal_sync_calls\":" << batches.size()
                  << ",\"seconds\":" << elapsed << ",\"mutations_per_second\":" << count / elapsed;
        Latencies(samples);
        std::cout << "}\n";
        Check(db->Close());
    }
}

void Populate(const std::filesystem::path& path, std::size_t count, bool prefix) {
    minilsm::Options options;
    options.prefix_compression = prefix;
    options.background_maintenance = false;
    options.memtable_bytes_limit = 128U * 1024U;
    std::unique_ptr<minilsm::DB> db;
    Check(minilsm::DB::Open(options, path, &db));
    for (std::size_t offset = 0U; offset < count; offset += 100U) {
        minilsm::WriteBatch batch;
        for (std::size_t i = offset; i < std::min(offset + 100U, count); ++i) batch.Put(Key(i), Value(i));
        Check(db->Write(minilsm::WriteOptions{false}, batch));
    }
    Check(db->Close());
}

void Reads(const std::filesystem::path& path, std::size_t count, std::size_t queries,
           std::size_t capacity, bool prefix, const std::string& experiment) {
    minilsm::Options options;
    options.block_cache_bytes = capacity;
    options.prefix_compression = prefix;
    std::unique_ptr<minilsm::DB> db;
    Check(minilsm::DB::Open(options, path, &db));
    std::mt19937_64 random(kSeed);
    std::vector<std::size_t> query_numbers;
    for (std::size_t i = 0U; i < queries; ++i) query_numbers.push_back(random() % count);
    std::vector<double> samples;
    samples.reserve(queries);
    db->ResetReadCounters();
    const auto begin = Clock::now();
    for (const auto number : query_numbers) {
        const auto key = Key(number);
        const auto call = Clock::now();
        const auto result = db->Get(key);
        samples.push_back(Seconds(Clock::now() - call) * 1e6);
        if (!result.status.ok() || result.value != Value(number)) throw std::runtime_error("Get result mismatch");
    }
    const double elapsed = Seconds(Clock::now() - begin);
    const auto counters = db->read_counters();
    const auto scan_begin = Clock::now();
    auto iterator = db->Scan(std::nullopt, std::nullopt);
    std::size_t scanned = 0U;
    while (iterator->Valid()) {
        if (iterator->key() != Key(scanned) || iterator->value() != Value(scanned)) throw std::runtime_error("scan mismatch");
        ++scanned;
        iterator->Next();
    }
    Check(iterator->status());
    if (scanned != count) throw std::runtime_error("scan count mismatch");
    const double scan_seconds = Seconds(Clock::now() - scan_begin);
    const auto bytes = TableBytes(path);
    const auto accesses = counters.block_cache_hits + counters.block_cache_misses;
    std::cout << "{\"experiment\":\"" << experiment << "\",\"keys\":" << count
              << ",\"queries\":" << queries << ",\"cache_bytes\":" << capacity
              << ",\"prefix_compression\":" << (prefix ? "true" : "false")
              << ",\"get_ops_per_second\":" << queries / elapsed
              << ",\"cache_hits\":" << counters.block_cache_hits
              << ",\"cache_misses\":" << counters.block_cache_misses
              << ",\"cache_evictions\":" << counters.block_cache_evictions
              << ",\"cache_hit_rate\":" << (accesses ? static_cast<double>(counters.block_cache_hits) / accesses : 0.0)
              << ",\"physical_block_reads\":" << counters.physical_block_reads
              << ",\"table_cache_hits\":" << counters.table_cache_hits
              << ",\"table_cache_misses\":" << counters.table_cache_misses
              << ",\"candidate_tables_per_get\":" << static_cast<double>(counters.sstable_files_considered) / queries
              << ",\"sstable_bytes\":" << bytes
              << ",\"sstable_bytes_per_key\":" << static_cast<double>(bytes) / count
              << ",\"live_sstable_space_amplification\":" << static_cast<double>(bytes) / (count * (Key(0).size() + kValueBytes))
              << ",\"scan_keys_per_second\":" << scanned / scan_seconds;
    Latencies(samples);
    std::cout << "}\n";
    iterator.reset();
    Check(db->Close());
}
#endif
}  // namespace

int main(int argc, char** argv) {
    const bool quick = argc == 2 && std::string(argv[1]) == "--quick";
    if (argc > 1 && !quick) { std::cerr << "Usage: cairnkv_experiments [--quick]\n"; return 2; }
    try {
        Workspace workspace;
        struct utsname system{};
        ::uname(&system);
        std::cout << std::fixed << std::setprecision(4)
                  << "{\"experiment\":\"context\",\"seed\":" << kSeed
                  << ",\"quick\":" << (quick ? "true" : "false")
                  << ",\"system\":\"" << system.sysname << ' ' << system.release << ' ' << system.machine
                  << "\",\"compiler\":\"" << __VERSION__
                  << "\",\"cache_state\":\"fresh application cache, uncontrolled warm OS cache\","
                     "\"caveat\":\"container filesystem; fsync completion is not a power-loss durability test; one trial\"}\n";
        Maintenance(workspace.path / "maintenance-sync", quick ? 3000U : 30000U, false);
#ifndef MINILSM_LEGACY
        Maintenance(workspace.path / "maintenance-background", quick ? 3000U : 30000U, true);
        Batching(workspace.path, quick ? 2000U : 20000U);
        const std::size_t count = quick ? 2000U : 30000U;
        const std::size_t queries = quick ? 5000U : 100000U;
        const auto compressed = workspace.path / "prefix-on";
        const auto plain = workspace.path / "prefix-off";
        Populate(compressed, count, true);
        Populate(plain, count, false);
        Reads(plain, count, queries, 0U, false, "prefix");
        Reads(compressed, count, queries, 0U, true, "prefix");
        for (const std::size_t capacity : {0U, 16U * 1024U, 256U * 1024U, 4U * 1024U * 1024U}) {
            Reads(compressed, count, queries, capacity, true, "cache");
        }
#endif
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
