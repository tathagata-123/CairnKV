#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "minilsm/status.hpp"
#include "sstable/sstable_reader.hpp"

namespace minilsm {

class BlockCache;

struct TableCacheStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
    std::size_t resident_entries = 0;
};

// The capacity bounds cached readers. An in-flight Get or Scan can pin an
// evicted reader, keeping its descriptor valid even after POSIX unlink.
class TableCache {
public:
    TableCache(std::filesystem::path directory, std::size_t capacity,
               std::shared_ptr<BlockCache> blocks);
    Status Get(std::uint64_t number, std::shared_ptr<SstableReader>* reader);
    void Erase(std::uint64_t number);
    void Clear();
    [[nodiscard]] TableCacheStats stats() const;

private:
    struct Entry {
        std::uint64_t number;
        std::shared_ptr<SstableReader> reader;
    };
    std::filesystem::path directory_;
    std::size_t capacity_;
    std::shared_ptr<BlockCache> blocks_;
    mutable std::mutex mutex_;
    std::list<Entry> lru_;
    std::unordered_map<std::uint64_t, std::list<Entry>::iterator> entries_;
    TableCacheStats stats_;
};

}  // namespace minilsm
