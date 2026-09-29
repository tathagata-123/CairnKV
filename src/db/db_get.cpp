#include "db/db_impl.hpp"

#include <limits>
#include <vector>
#include "common/internal_key.hpp"
#include "minilsm/format_limits.hpp"
#include "sstable/sstable_iterator.hpp"

namespace minilsm {

Status DBImpl::LookupSstableFile(const std::shared_ptr<SstableReader>& reader,
                                std::string_view user_key, MemTableLookup* lookup,
                                ReadCounters* counters) const {
    *lookup = MemTableLookup{};
    ++counters->sstable_files_considered;
    if (reader->has_bloom_filter()) {
        ++counters->bloom_filter_probes;
        if (!reader->MayContain(user_key)) {
            ++counters->bloom_definite_negatives;
            return Status::Ok();
        }
        ++counters->bloom_possible_positives;
    }
    const InternalKey target{std::string(user_key),
        std::numeric_limits<std::uint64_t>::max(), RecordType::kTombstone};
    if (reader->FindCandidateBlock(target) == reader->index().size()) return Status::Ok();
    ++counters->sstable_data_block_reads;
    SstableIterator iterator(reader.get());
    Status status = iterator.Seek(target);
    if (!status.ok()) return status;
    if (!iterator.Valid() || iterator.key().user_key != user_key) return Status::Ok();
    if (iterator.key().record_type == RecordType::kTombstone) {
        lookup->state = MemTableLookupState::kTombstone;
    } else {
        lookup->state = MemTableLookupState::kValue;
        lookup->value.assign(iterator.value());
    }
    return Status::Ok();
}

LookupResult DBImpl::Get(std::string_view key) const {
    if (key.size() > kMaxUserKeyBytes) {
        return {Status::InvalidArgument("user key exceeds the 1 MiB limit"), std::nullopt};
    }
    std::vector<std::shared_ptr<SstableReader>> readers;
    {
        std::lock_guard lock(mutex_);
        if (closed_) return {Status::IOError("database is closed"), std::nullopt};
        MemTableLookup memory = memtable_.Lookup(key);
        if (memory.state == MemTableLookupState::kNotPresent && immutable_) memory = immutable_->Lookup(key);
        if (memory.state == MemTableLookupState::kValue) return {Status::Ok(), std::move(memory.value)};
        if (memory.state == MemTableLookupState::kTombstone) return {Status::NotFound("memtable tombstone"), std::nullopt};
        // Capture in freshness order, before any compaction may unlink inputs.
        auto pin = [&](const FileMetaData& file) {
            std::shared_ptr<SstableReader> reader;
            Status status = table_cache_.Get(file.file_number, &reader);
            if (status.ok()) readers.push_back(std::move(reader));
            return status;
        };
        for (auto it = version_.levels[0].rbegin(); it != version_.levels[0].rend(); ++it) {
            if (CompareUserKeys(key, it->smallest_user_key) < 0 || CompareUserKeys(key, it->largest_user_key) > 0) continue;
            Status status = pin(*it);
            if (!status.ok()) return {status, std::nullopt};
        }
        for (std::size_t level = 1U; level < kLevelCount; ++level) {
            auto candidate = FindFileByUserKeyRange(version_.levels[level], key);
            if (!candidate) continue;
            Status status = pin(version_.levels[level][*candidate]);
            if (!status.ok()) return {status, std::nullopt};
        }
    }
    // pread, checksums and decoding run independently for concurrent readers.
    ReadCounters counters;
    LookupResult result{Status::NotFound("user key is not present"), std::nullopt};
    for (const auto& reader : readers) {
        MemTableLookup lookup;
        Status status = LookupSstableFile(reader, key, &lookup, &counters);
        if (!status.ok()) { result.status = status; break; }
        if (lookup.state == MemTableLookupState::kTombstone) break;
        if (lookup.state == MemTableLookupState::kValue) {
            result = {Status::Ok(), std::move(lookup.value)};
            break;
        }
    }
    std::lock_guard lock(mutex_);
    read_counters_.sstable_files_considered += counters.sstable_files_considered;
    read_counters_.bloom_filter_probes += counters.bloom_filter_probes;
    read_counters_.bloom_definite_negatives += counters.bloom_definite_negatives;
    read_counters_.bloom_possible_positives += counters.bloom_possible_positives;
    read_counters_.sstable_data_block_reads += counters.sstable_data_block_reads;
    return result;
}

ReadCounters DBImpl::read_counters() const {
    std::lock_guard lock(mutex_);
    ReadCounters result = read_counters_;
    const auto blocks = block_cache_->Stats();
    const auto tables = table_cache_.stats();
    result.block_cache_hits = blocks.hits - counter_base_.block_cache_hits;
    result.block_cache_misses = blocks.misses - counter_base_.block_cache_misses;
    result.block_cache_evictions = blocks.evictions - counter_base_.block_cache_evictions;
    result.physical_block_reads = blocks.physical_reads - counter_base_.physical_block_reads;
    result.table_cache_hits = tables.hits - counter_base_.table_cache_hits;
    result.table_cache_misses = tables.misses - counter_base_.table_cache_misses;
    return result;
}

void DBImpl::ResetReadCounters() {
    std::lock_guard lock(mutex_);
    read_counters_ = ReadCounters{};
    const auto blocks = block_cache_->Stats();
    const auto tables = table_cache_.stats();
    counter_base_.block_cache_hits = blocks.hits;
    counter_base_.block_cache_misses = blocks.misses;
    counter_base_.block_cache_evictions = blocks.evictions;
    counter_base_.physical_block_reads = blocks.physical_reads;
    counter_base_.table_cache_hits = tables.hits;
    counter_base_.table_cache_misses = tables.misses;
}
}  // namespace minilsm
