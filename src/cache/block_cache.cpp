#include "cache/block_cache.hpp"

namespace minilsm {

std::shared_ptr<const DataBlockReader> BlockCache::Lookup(std::uint64_t file, std::uint64_t offset) {
    std::lock_guard lock(mutex_);
    const auto found = index_.find(Key{file, offset});
    if (found == index_.end()) { ++stats_.misses; return {}; }
    ++stats_.hits;
    lru_.splice(lru_.begin(), lru_, found->second);
    return found->second->block;
}

void BlockCache::Insert(std::uint64_t file, std::uint64_t offset,
                        std::shared_ptr<const DataBlockReader> block) {
    if (!block) return;
    const std::size_t charge = block->MemoryUsage();
    std::lock_guard lock(mutex_);
    if (charge > capacity_) return;
    const Key key{file, offset};
    const auto found = index_.find(key);
    if (found != index_.end()) {
        lru_.splice(lru_.begin(), lru_, found->second);
        return;
    }
    while (!lru_.empty() && charge > capacity_ - stats_.resident_bytes) {
        const auto& old = lru_.back();
        stats_.resident_bytes -= old.bytes;
        index_.erase(old.key);
        lru_.pop_back();
        ++stats_.evictions;
    }
    lru_.push_front(Entry{key, std::move(block), charge});
    index_.emplace(key, lru_.begin());
    stats_.resident_bytes += charge;
}

void BlockCache::Erase(std::uint64_t file) {
    std::lock_guard lock(mutex_);
    for (auto entry = lru_.begin(); entry != lru_.end();) {
        if (entry->key.file != file) { ++entry; continue; }
        stats_.resident_bytes -= entry->bytes;
        index_.erase(entry->key);
        entry = lru_.erase(entry);
    }
}

void BlockCache::Clear() {
    std::lock_guard lock(mutex_);
    index_.clear(); lru_.clear(); stats_.resident_bytes = 0;
}

void BlockCache::RecordRead(std::size_t bytes) {
    std::lock_guard lock(mutex_);
    ++stats_.physical_reads;
    stats_.bytes_read += bytes;
}

BlockCacheStats BlockCache::Stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

}  // namespace minilsm
