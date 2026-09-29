#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include "cache/block_cache.hpp"
#include "sstable/sstable_builder.hpp"
#include "sstable/sstable_reader.hpp"
#include "test_harness.hpp"

namespace {
using namespace minilsm;

std::shared_ptr<const DataBlockReader> Block() {
    DataBlockBuilder builder(true);
    builder.Add({"key", 1, RecordType::kValue}, "value");
    std::string bytes; builder.Finish(&bytes);
    auto block = std::make_shared<DataBlockReader>();
    DataBlockReader::Decode(bytes, block.get());
    return block;
}

bool TestCapacityEvictionAndPins() {
    const auto block = Block();
    BlockCache cache(block->MemoryUsage() * 2);
    MINILSM_CHECK(!cache.Lookup(1, 0));
    cache.Insert(1, 0, block); cache.Insert(2, 0, block);
    auto pin = cache.Lookup(1, 0); // file 2 is least recently used
    cache.Insert(3, 0, block);
    MINILSM_CHECK(!cache.Lookup(2, 0));
    MINILSM_CHECK(cache.Lookup(1, 0) == pin);
    MINILSM_CHECK(cache.Stats().evictions == 1);
    MINILSM_CHECK(cache.Stats().resident_bytes <= block->MemoryUsage() * 2);
    cache.Erase(1);
    MINILSM_CHECK(!cache.Lookup(1, 0));
    MINILSM_CHECK(pin->entries()[0].value == "value");
    cache.Insert(4, 0, block); // different file number cannot reuse retired entry
    MINILSM_CHECK(cache.Lookup(4, 0));
    cache.Clear();
    MINILSM_CHECK(cache.Stats().resident_bytes == 0);
    BlockCache too_small(block->MemoryUsage() - 1);
    too_small.Insert(1, 0, block);
    MINILSM_CHECK(!too_small.Lookup(1, 0));
    BlockCache disabled(0);
    disabled.Insert(1, 0, block);
    MINILSM_CHECK(!disabled.Lookup(1, 0));
    MINILSM_CHECK(disabled.Stats().resident_bytes == 0);
    return true;
}

bool TestConcurrentCacheOperations() {
    const auto block = Block();
    BlockCache cache(block->MemoryUsage() * 3);
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 8; ++worker) {
        workers.emplace_back([&cache, &block, worker] {
            for (int i = 0; i < 1000; ++i) {
                const std::uint64_t key = static_cast<std::uint64_t>((i + worker) % 8);
                auto pin = cache.Lookup(key, 0);
                cache.Insert(key, 0, block);
                if (i % 7 == 0) cache.Erase((key + 1) % 8);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    const auto stats = cache.Stats();
    MINILSM_CHECK(stats.hits + stats.misses == 8000);
    MINILSM_CHECK(stats.resident_bytes <= block->MemoryUsage() * 3);
    return true;
}

bool TestReaderReuseAndPhysicalReads() {
    const auto path = std::filesystem::temp_directory_path() /
        ("minilsm-block-cache-" + std::to_string(getpid()) + ".sst");
    std::filesystem::remove(path);
    std::unique_ptr<SstableBuilder> builder;
    MINILSM_CHECK(SstableBuilder::Create(path, 1024, &builder, true).ok());
    MINILSM_CHECK(builder->Add({"common-prefix:1", 1, RecordType::kValue}, "value").ok());
    MINILSM_CHECK(builder->Finish().ok());
    std::unique_ptr<SstableReader> reader;
    MINILSM_CHECK(SstableReader::Open(path, &reader).ok());
    auto cache = std::make_shared<BlockCache>(4096);
    reader->SetBlockCache(cache, 7);
    std::shared_ptr<const DataBlockReader> first, second;
    MINILSM_CHECK(reader->ReadSharedDataBlock(0, &first).ok());
    MINILSM_CHECK(reader->ReadSharedDataBlock(0, &second).ok());
    MINILSM_CHECK(first == second);
    MINILSM_CHECK(cache->Stats().physical_reads == 1);
    MINILSM_CHECK(cache->Stats().misses == 1 && cache->Stats().hits == 1);
    MINILSM_CHECK(cache->Stats().bytes_read > 0);
    cache->Erase(7);
    MINILSM_CHECK(reader->ReadSharedDataBlock(0, &second).ok());
    MINILSM_CHECK(cache->Stats().physical_reads == 2);
    std::filesystem::remove(path);
    MINILSM_CHECK(first->entries()[0].value == "value");
    return true;
}
}  // namespace

int main() {
    MINILSM_RUN(TestCapacityEvictionAndPins);
    MINILSM_RUN(TestReaderReuseAndPhysicalReads);
    MINILSM_RUN(TestConcurrentCacheOperations);
    return 0;
}
