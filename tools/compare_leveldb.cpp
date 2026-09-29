// One identical workload, compiled for either engine. LevelDB API references:
// https://github.com/google/leveldb/blob/main/doc/index.md
// Build CairnKV normally; build the optional peer with -DCAIRNKV_USE_LEVELDB.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

#ifdef CAIRNKV_USE_LEVELDB
#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#else
#include "minilsm/db.hpp"
#endif

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t kCacheBytes = 4U * 1024U * 1024U;
std::string Key(std::size_t number) {
    char key[64];
    std::snprintf(key, sizeof(key), "customer:region-east:%012zu", number);
    return key;
}
double Seconds(Clock::duration time) { return std::chrono::duration<double>(time).count(); }
void Percentiles(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    for (std::size_t percentile : {50U, 95U, 99U}) {
        const auto position = (samples.size() * percentile + 99U) / 100U - 1U;
        std::cout << ",\"p" << percentile << "_us\":" << samples[position];
    }
}

// This small adapter only keeps the workloads identical. Each branch uses
// that engine's ordinary public API; it is not shared engine infrastructure.
class Engine {
public:
    explicit Engine(const std::filesystem::path& path) : path_(path) {
#ifdef CAIRNKV_USE_LEVELDB
        options_.create_if_missing = true;
        options_.compression = leveldb::kNoCompression;
        options_.write_buffer_size = 128U * 1024U;
        options_.block_size = 4096U;
        cache_.reset(leveldb::NewLRUCache(kCacheBytes));
        filter_.reset(leveldb::NewBloomFilterPolicy(10));
        options_.block_cache = cache_.get();
        options_.filter_policy = filter_.get();
#else
        options_.memtable_bytes_limit = 128U * 1024U;
        options_.sstable_block_target_bytes = 4096U;
        options_.block_cache_bytes = kCacheBytes;
#endif
        Open();
    }
    void Open() {
#ifdef CAIRNKV_USE_LEVELDB
        leveldb::DB* db = nullptr;
        Check(leveldb::DB::Open(options_, path_.string(), &db));
        db_.reset(db);
#else
        Check(minilsm::DB::Open(options_, path_, &db_));
#endif
    }
    void Put(const std::string& key, const std::string& value) {
#ifdef CAIRNKV_USE_LEVELDB
        leveldb::WriteOptions write;
        write.sync = true;
        Check(db_->Put(write, key, value));
#else
        Check(db_->Put(key, value));
#endif
    }
    void PrepareReads() {
#ifdef CAIRNKV_USE_LEVELDB
        // LevelDB has no public Flush. This explicit full-range compaction
        // forces an SSTable dataset; it is outside all timed read/write loops.
        db_->CompactRange(nullptr, nullptr);
        std::string stats;
        if (db_->GetProperty("leveldb.stats", &stats)) std::cerr << "LevelDB native stats before read preparation close:\n" << stats << '\n';
        db_.reset();
#else
        Check(db_->Close());
        db_.reset();
#endif
        Open();
    }
    void Get(const std::string& key, const std::string& expected) {
#ifdef CAIRNKV_USE_LEVELDB
        std::string value;
        Check(db_->Get(leveldb::ReadOptions{}, key, &value));
        if (value != expected) throw std::runtime_error("Get mismatch");
#else
        const auto result = db_->Get(key);
        Check(result.status);
        if (result.value != expected) throw std::runtime_error("Get mismatch");
#endif
    }
    std::size_t Scan() {
        std::size_t count = 0U;
#ifdef CAIRNKV_USE_LEVELDB
        std::unique_ptr<leveldb::Iterator> iterator(db_->NewIterator(leveldb::ReadOptions{}));
        iterator->SeekToFirst();
#else
        auto iterator = db_->Scan(std::nullopt, std::nullopt);
#endif
        while (iterator->Valid()) { ++count; iterator->Next(); }
        Check(iterator->status());
        return count;
    }
    void Close() {
#ifndef CAIRNKV_USE_LEVELDB
        Check(db_->Close());
#endif
        db_.reset();
    }
private:
#ifdef CAIRNKV_USE_LEVELDB
    static void Check(const leveldb::Status& status) {
        if (!status.ok()) throw std::runtime_error(status.ToString());
    }
    leveldb::Options options_;
    std::unique_ptr<leveldb::Cache> cache_;
    std::unique_ptr<const leveldb::FilterPolicy> filter_;
    std::unique_ptr<leveldb::DB> db_;
#else
    static void Check(const minilsm::Status& status) {
        if (!status.ok()) throw std::runtime_error(status.message());
    }
    minilsm::Options options_;
    std::unique_ptr<minilsm::DB> db_;
#endif
    std::filesystem::path path_;
};
}  // namespace

int main(int argc, char** argv) {
    const bool quick = argc == 2 && std::string(argv[1]) == "--quick";
    if (argc > 1 && !quick) { std::cerr << "Usage: compare_[minilsm|leveldb] [--quick]\n"; return 2; }
    const std::size_t count = quick ? 2000U : 30000U;
    const std::size_t queries = quick ? 5000U : 100000U;
    std::string pattern = (std::filesystem::temp_directory_path() / "lsm-comparison-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end()); writable.push_back('\0');
    const char* created = ::mkdtemp(writable.data());
    if (created == nullptr) return 1;
    const std::filesystem::path path = created;
    try {
        Engine db(path);
        const std::string value(100U, 'v');
        std::vector<std::string> keys;
        for (std::size_t i = 0U; i < count; ++i) keys.push_back(Key(i));
        std::vector<double> write_samples;
        write_samples.reserve(count);
        const auto write_begin = Clock::now();
        for (const auto& key : keys) {
            const auto begin = Clock::now(); db.Put(key, value);
            write_samples.push_back(Seconds(Clock::now() - begin) * 1e6);
        }
        const double write_seconds = Seconds(Clock::now() - write_begin);
        const auto preparation_begin = Clock::now();
        db.PrepareReads();
        const double preparation_seconds = Seconds(Clock::now() - preparation_begin);
        std::mt19937_64 random(20260929U);
        std::vector<std::size_t> query_numbers;
        for (std::size_t i = 0U; i < queries; ++i) query_numbers.push_back(random() % count);
        std::vector<double> get_samples;
        get_samples.reserve(queries);
        const auto read_begin = Clock::now();
        for (const auto number : query_numbers) {
            const auto begin = Clock::now(); db.Get(keys[number], value);
            get_samples.push_back(Seconds(Clock::now() - begin) * 1e6);
        }
        const double read_seconds = Seconds(Clock::now() - read_begin);
        const auto scan_begin = Clock::now();
        if (db.Scan() != count) throw std::runtime_error("scan count mismatch");
        const double scan_seconds = Seconds(Clock::now() - scan_begin);
        db.Close();
        std::uint64_t bytes = 0U;
        for (const auto& file : std::filesystem::directory_iterator(path)) if (file.is_regular_file()) bytes += file.file_size();
        std::cout << std::fixed << std::setprecision(4)
#ifdef CAIRNKV_USE_LEVELDB
                  << "{\"engine\":\"LevelDB\",\"version\":\"" << leveldb::kMajorVersion << '.' << leveldb::kMinorVersion << "\","
#else
                  << "{\"engine\":\"CairnKV\","
#endif
                  << "\"keys\":" << count << ",\"sync\":true,\"block_bytes\":4096,\"cache_bytes\":" << kCacheBytes
                  << ",\"prefix_keys\":true,\"extra_compressor\":false,\"write_ops_per_second\":" << count / write_seconds
                  << ",\"read_preparation_seconds\":" << preparation_seconds
                  << ",\"get_ops_per_second\":" << queries / read_seconds
                  << ",\"scan_keys_per_second\":" << count / scan_seconds
                  << ",\"database_bytes_after_close\":" << bytes
                  << ",\"write_amplification\":null,\"write_latency\":{";
        std::cout << "\"samples\":" << write_samples.size(); Percentiles(write_samples);
        std::cout << "},\"get_latency\":{\"samples\":" << get_samples.size(); Percentiles(get_samples);
        std::cout << "}}\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::error_code ignored; std::filesystem::remove_all(path, ignored); return 1;
    }
    std::error_code ignored; std::filesystem::remove_all(path, ignored);
    return 0;
}
