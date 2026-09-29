#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include "sstable/block.hpp"

namespace minilsm {

struct BlockCacheStats {
    std::uint64_t hits = 0, misses = 0, evictions = 0;
    std::uint64_t physical_reads = 0, bytes_read = 0;
    std::size_t resident_bytes = 0;
};

// Capacity covers decoded resident blocks, not blocks pinned by active readers.
class BlockCache {
public:
    explicit BlockCache(std::size_t capacity_bytes) : capacity_(capacity_bytes) {}
    std::shared_ptr<const DataBlockReader> Lookup(std::uint64_t file, std::uint64_t offset);
    void Insert(std::uint64_t file, std::uint64_t offset, std::shared_ptr<const DataBlockReader> block);
    void Erase(std::uint64_t file);
    void Clear();
    void RecordRead(std::size_t bytes);
    [[nodiscard]] BlockCacheStats Stats() const;
private:
    struct Key {
        std::uint64_t file, offset;
        bool operator==(const Key&) const = default;
    };
    struct Hash {
        std::size_t operator()(const Key& key) const {
            return std::hash<std::uint64_t>{}(key.file) ^
                (std::hash<std::uint64_t>{}(key.offset) << 1U);
        }
    };
    struct Entry { Key key; std::shared_ptr<const DataBlockReader> block; std::size_t bytes; };
    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::list<Entry> lru_;
    std::unordered_map<Key, std::list<Entry>::iterator, Hash> index_;
    BlockCacheStats stats_;
};

}  // namespace minilsm
