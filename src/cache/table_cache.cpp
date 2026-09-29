#include "cache/table_cache.hpp"

#include <string>
#include <utility>

#include "common/file_names.hpp"

namespace minilsm {

TableCache::TableCache(std::filesystem::path directory, std::size_t capacity,
                       std::shared_ptr<BlockCache> blocks)
    : directory_(std::move(directory)), capacity_(capacity),
      blocks_(std::move(blocks)) {}

Status TableCache::Get(std::uint64_t number,
                       std::shared_ptr<SstableReader>* reader) {
    if (reader == nullptr) {
        return Status::InvalidArgument("table cache received a null output");
    }
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(number);
    if (found != entries_.end()) {
        ++stats_.hits;
        lru_.splice(lru_.begin(), lru_, found->second);
        *reader = found->second->reader;
        return Status::Ok();
    }
    ++stats_.misses;
    std::string name;
    Status status = MakeSstableFileName(number, false, &name);
    if (!status.ok()) {
        return status;
    }
    std::unique_ptr<SstableReader> opened;
    status = SstableReader::Open(directory_ / name, &opened);
    if (!status.ok()) {
        return status;
    }
    opened->SetBlockCache(blocks_, number);
    *reader = std::shared_ptr<SstableReader>(std::move(opened));
    if (capacity_ == 0) {
        return Status::Ok();
    }
    lru_.push_front({number, *reader});
    entries_.emplace(number, lru_.begin());
    if (entries_.size() > capacity_) {
        entries_.erase(lru_.back().number);
        lru_.pop_back();
        ++stats_.evictions;
    }
    return Status::Ok();
}

void TableCache::Erase(std::uint64_t number) {
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(number);
    if (found != entries_.end()) {
        lru_.erase(found->second);
        entries_.erase(found);
    }
}

void TableCache::Clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
    lru_.clear();
}

TableCacheStats TableCache::stats() const {
    std::lock_guard lock(mutex_);
    TableCacheStats result = stats_;
    result.resident_entries = entries_.size();
    return result;
}

}  // namespace minilsm
