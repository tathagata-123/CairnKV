#include "memtable/memtable.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "minilsm/format_limits.hpp"

namespace minilsm {

Status MemTable::Add(
    InternalKey internal_key,
    std::string value) {
    if (internal_key.user_key.size() > kMaxUserKeyBytes) {
        return Status::InvalidArgument(
            "memtable user key exceeds 1 MiB");
    }
    if (value.size() > kMaxValueBytes) {
        return Status::InvalidArgument(
            "memtable value exceeds 64 MiB");
    }
    if (internal_key.sequence_number == 0U) {
        return Status::InvalidArgument(
            "memtable sequence number must be nonzero");
    }
    if (internal_key.record_type != RecordType::kValue &&
        internal_key.record_type != RecordType::kTombstone) {
        return Status::InvalidArgument(
            "memtable record type is invalid");
    }
    if (internal_key.record_type == RecordType::kTombstone &&
        !value.empty()) {
        return Status::InvalidArgument(
            "memtable tombstone must not contain a value");
    }

    const std::size_t record_bytes =
        kMinInternalKeyBytes + internal_key.user_key.size() + value.size();
    if (record_bytes >
        std::numeric_limits<std::size_t>::max() -
            approximate_memory_usage_) {
        return Status::InvalidArgument(
            "memtable byte accounting overflows size_t");
    }

    const InternalKey first_at_sequence{
        internal_key.user_key,
        internal_key.sequence_number,
        RecordType::kTombstone,
    };
    const auto existing = records_.lower_bound(first_at_sequence);
    if (existing != records_.end() &&
        existing->first.user_key == internal_key.user_key &&
        existing->first.sequence_number ==
            internal_key.sequence_number) {
        return Status::Corruption(
            "memtable contains duplicate user-key sequence");
    }

    records_.emplace(std::move(internal_key), std::move(value));
    approximate_memory_usage_ += record_bytes;
    return Status::Ok();
}

MemTableLookup MemTable::Lookup(
    std::string_view user_key) const {
    const InternalKey newest_possible{
        std::string(user_key),
        std::numeric_limits<std::uint64_t>::max(),
        RecordType::kTombstone,
    };
    const auto found = records_.lower_bound(newest_possible);
    if (found == records_.end() ||
        found->first.user_key != user_key) {
        return MemTableLookup{};
    }
    if (found->first.record_type == RecordType::kTombstone) {
        return MemTableLookup{
            MemTableLookupState::kTombstone, {}};
    }
    return MemTableLookup{
        MemTableLookupState::kValue, found->second};
}

std::size_t MemTable::size() const {
    return records_.size();
}

bool MemTable::empty() const {
    return records_.empty();
}

std::size_t MemTable::approximate_memory_usage() const {
    return approximate_memory_usage_;
}

const MemTable::RecordMap& MemTable::records() const {
    return records_;
}

void MemTable::Clear() {
    records_.clear();
    approximate_memory_usage_ = 0U;
}

}  // namespace minilsm
