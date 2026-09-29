#include "sstable/block.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/checksum.hpp"
#include "common/coding.hpp"
#include "minilsm/format_limits.hpp"
#include "sstable/sstable_format.hpp"

namespace minilsm {

namespace {

constexpr std::string_view kPrefixMagic = "MDB2";
constexpr std::uint32_t kRestartInterval = 16U;

bool SameUserKeyAndSequence(
    const InternalKey& left,
    const InternalKey& right) {
    return left.user_key == right.user_key &&
           left.sequence_number == right.sequence_number;
}

Status EncodeDataEntry(
    const InternalKey& internal_key,
    std::string_view value,
    std::string* entry) {
    if (value.size() > kMaxValueBytes) {
        return Status::InvalidArgument(
            "data-block value exceeds 64 MiB");
    }
    if (internal_key.record_type == RecordType::kTombstone &&
        !value.empty()) {
        return Status::InvalidArgument(
            "data-block tombstone must not contain a value");
    }

    std::string encoded_internal_key;
    Status status =
        EncodeInternalKey(internal_key, &encoded_internal_key);
    if (!status.ok()) {
        return status;
    }

    const std::size_t fixed_bytes = 8U;
    if (encoded_internal_key.size() >
            std::numeric_limits<std::size_t>::max() - fixed_bytes ||
        value.size() >
            std::numeric_limits<std::size_t>::max() - fixed_bytes -
                encoded_internal_key.size()) {
        return Status::InvalidArgument("data-block entry size overflows");
    }

    std::string result;
    result.reserve(
        fixed_bytes + encoded_internal_key.size() + value.size());
    PutFixed32(
        &result,
        static_cast<std::uint32_t>(encoded_internal_key.size()));
    result.append(encoded_internal_key);
    PutFixed32(&result, static_cast<std::uint32_t>(value.size()));
    result.append(value);
    *entry = std::move(result);
    return Status::Ok();
}

Status FramedBlockSizeMatches(
    std::string_view physical_block,
    std::uint32_t body_size) {
    const std::uint64_t expected_size =
        8U + static_cast<std::uint64_t>(body_size);
    if (expected_size != physical_block.size()) {
        return Status::Corruption(
            "data-block body size does not match its physical size");
    }
    return Status::Ok();
}

}  // namespace

Status DataBlockBuilder::Add(
    const InternalKey& internal_key,
    std::string_view value) {
    std::string encoded_entry;
    Status status = EncodeDataEntry(internal_key, value, &encoded_entry);
    if (!status.ok()) {
        return status;
    }

    if (last_key_.has_value()) {
        const InternalKeyComparator compare;
        if (SameUserKeyAndSequence(*last_key_, internal_key)) {
            return Status::InvalidArgument(
                "data-block keys repeat a user-key sequence");
        }
        if (compare.Compare(*last_key_, internal_key) >= 0) {
            return Status::InvalidArgument(
                "data-block keys must increase strictly");
        }
    }
    if (entry_count_ == std::numeric_limits<std::uint32_t>::max()) {
        return Status::InvalidArgument(
            "data-block entry count exceeds uint32");
    }

    const std::size_t uncompressed_size = encoded_entry.size();
    if (uncompressed_size > kMaxDataBlockBodyBytes - kDataBlockHeaderBytes - uncompressed_bytes_)
        return Status::InvalidArgument("decoded data block exceeds 66 MiB limit");
    const bool restart = entry_count_ % kRestartInterval == 0U;
    if (prefix_compression_) {
        std::size_t shared = 0;
        if (!restart) {
            const std::string& previous = last_key_->user_key;
            while (shared < previous.size() && shared < internal_key.user_key.size() &&
                   previous[shared] == internal_key.user_key[shared]) ++shared;
        }
        encoded_entry.clear();
        PutFixed32(&encoded_entry, static_cast<std::uint32_t>(shared));
        PutFixed32(&encoded_entry, static_cast<std::uint32_t>(internal_key.user_key.size() - shared));
        PutFixed32(&encoded_entry, static_cast<std::uint32_t>(value.size()));
        PutFixed64(&encoded_entry, internal_key.sequence_number);
        encoded_entry.push_back(static_cast<char>(internal_key.record_type));
        encoded_entry.append(internal_key.user_key, shared, std::string::npos);
        encoded_entry.append(value);
    }
    const std::size_t trailer = prefix_compression_
        ? 4U + 4U * (restart_offsets_.size() + (restart ? 1U : 0U)) : 0U;
    if (trailer > kMaxDataBlockBodyBytes - kDataBlockHeaderBytes)
        return Status::InvalidArgument("data-block restart table exceeds limit");
    const std::size_t available_body_bytes =
        kMaxDataBlockBodyBytes - kDataBlockHeaderBytes - trailer;
    if (entries_bytes_.size() > available_body_bytes ||
        encoded_entry.size() >
            available_body_bytes - entries_bytes_.size()) {
        return Status::InvalidArgument(
            "data-block body exceeds the 66 MiB limit");
    }

    if (prefix_compression_ && restart)
        restart_offsets_.push_back(static_cast<std::uint32_t>(entries_bytes_.size()));
    entries_bytes_.append(encoded_entry);
    uncompressed_bytes_ += uncompressed_size;
    ++entry_count_;
    last_key_ = internal_key;
    return Status::Ok();
}

Status DataBlockBuilder::Finish(std::string* block) const {
    if (block == nullptr) {
        return Status::InvalidArgument(
            "data-block finish received a null output");
    }
    if (empty()) {
        return Status::InvalidArgument(
            "a data block must contain at least one entry");
    }

    std::string body(prefix_compression_ ? kPrefixMagic : kDataBlockMagic);
    PutFixed16(&body, prefix_compression_ ? 2U : kSstableFormatVersion);
    PutFixed16(&body, 0U);
    PutFixed32(&body, entry_count_);
    body.append(entries_bytes_);
    if (prefix_compression_) {
        for (const auto offset : restart_offsets_) PutFixed32(&body, offset);
        PutFixed32(&body, static_cast<std::uint32_t>(restart_offsets_.size()));
    }
    if (body.size() < kMinDataBlockBodyBytes ||
        body.size() > kMaxDataBlockBodyBytes) {
        return Status::InvalidArgument(
            "data-block body is outside version 1 bounds");
    }

    std::string result;
    result.reserve(body.size() + 8U);
    PutFixed32(&result, static_cast<std::uint32_t>(body.size()));
    result.append(body);
    PutFixed32(&result, Crc32c(result));
    *block = std::move(result);
    return Status::Ok();
}

bool DataBlockBuilder::empty() const {
    return entry_count_ == 0U;
}

std::uint32_t DataBlockBuilder::entry_count() const {
    return entry_count_;
}

std::size_t DataBlockBuilder::current_physical_size() const {
    return 8U + kDataBlockHeaderBytes + entries_bytes_.size() +
        (prefix_compression_ ? 4U + 4U * restart_offsets_.size() : 0U);
}

const InternalKey& DataBlockBuilder::last_key() const {
    assert(last_key_.has_value());
    return *last_key_;
}

Status DataBlockReader::Decode(
    std::string_view physical_block,
    DataBlockReader* reader) {
    if (reader == nullptr) {
        return Status::InvalidArgument(
            "data-block decode received a null output");
    }
    if (physical_block.size() < kMinDataBlockPhysicalBytes ||
        physical_block.size() > kMaxDataBlockPhysicalBytes) {
        return Status::Corruption(
            "data-block physical size is outside version 1 bounds");
    }

    std::string_view size_input = physical_block;
    std::uint32_t body_size = 0U;
    Status status = GetFixed32(&size_input, &body_size);
    if (!status.ok() ||
        body_size < kMinDataBlockBodyBytes ||
        body_size > kMaxDataBlockBodyBytes) {
        return Status::Corruption(
            "data-block body size is outside version 1 bounds");
    }
    status = FramedBlockSizeMatches(physical_block, body_size);
    if (!status.ok()) {
        return status;
    }

    std::string_view checksum_field =
        physical_block.substr(physical_block.size() - 4U);
    std::uint32_t stored_checksum = 0U;
    status = GetFixed32(&checksum_field, &stored_checksum);
    if (!status.ok() ||
        !VerifyCrc32c(
            physical_block.substr(0U, physical_block.size() - 4U),
            stored_checksum)) {
        return Status::Corruption("data-block checksum mismatch");
    }

    std::string_view body = physical_block.substr(4U, body_size);
    const bool prefix = body.substr(0U, kPrefixMagic.size()) == kPrefixMagic;
    if (!prefix && body.substr(0U, kDataBlockMagic.size()) != kDataBlockMagic) {
        return Status::Corruption("data-block magic is invalid");
    }
    std::string_view fields = body.substr(kDataBlockMagic.size());
    std::uint16_t version = 0U;
    std::uint16_t reserved = 0U;
    std::uint32_t entry_count = 0U;
    status = GetFixed16(&fields, &version);
    if (status.ok()) {
        status = GetFixed16(&fields, &reserved);
    }
    if (status.ok()) {
        status = GetFixed32(&fields, &entry_count);
    }
    if (!status.ok() || version != (prefix ? 2U : kSstableFormatVersion) ||
        reserved != 0U || entry_count == 0U) {
        return Status::Corruption(
            "data-block header fields are invalid");
    }
    if (entry_count > fields.size() / kMinDataBlockEntryBytes) {
        return Status::Corruption(
            "data-block entry count exceeds its bounded body");
    }

    std::vector<std::uint32_t> restarts;
    if (prefix) {
        std::string_view tail = fields.substr(fields.size() - 4U);
        std::uint32_t count = 0;
        if (!GetFixed32(&tail, &count).ok() || count != (entry_count - 1U) / kRestartInterval + 1U ||
            count > (fields.size() - 4U) / 4U)
            return Status::Corruption("invalid restart count");
        const std::size_t entries_size = fields.size() - 4U - 4U * count;
        tail = fields.substr(entries_size, 4U * count);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t offset = 0;
            if (!GetFixed32(&tail, &offset).ok() || offset >= entries_size ||
                (i == 0 ? offset != 0 : offset <= restarts.back()))
                return Status::Corruption("invalid restart offset");
            restarts.push_back(offset);
        }
        fields = fields.substr(0U, entries_size);
    }
    const std::size_t entries_size = fields.size();
    std::vector<DataBlockEntry> entries;
    entries.reserve(entry_count);
    std::optional<InternalKey> previous_key;
    std::size_t uncompressed_bytes = 0;
    const InternalKeyComparator compare;
    for (std::uint32_t index = 0U; index < entry_count; ++index) {
        InternalKey internal_key;
        std::uint32_t value_size = 0U;
        if (prefix) {
            const std::size_t offset = entries_size - fields.size();
            const bool restart = index % kRestartInterval == 0U;
            std::uint32_t shared = 0, unshared = 0;
            if (!GetFixed32(&fields, &shared).ok() ||
                !GetFixed32(&fields, &unshared).ok() ||
                !GetFixed32(&fields, &value_size).ok() ||
                !GetFixed64(&fields, &internal_key.sequence_number).ok() || fields.empty())
                return Status::Corruption("truncated prefix entry");
            internal_key.record_type = static_cast<RecordType>(static_cast<unsigned char>(fields.front()));
            fields.remove_prefix(1U);
            if ((restart && (shared != 0U || offset != restarts[index / kRestartInterval])) ||
                shared > (previous_key ? previous_key->user_key.size() : 0U) ||
                shared > kMaxUserKeyBytes || unshared > kMaxUserKeyBytes - shared ||
                unshared > fields.size() || internal_key.sequence_number == 0U ||
                (internal_key.record_type != RecordType::kValue && internal_key.record_type != RecordType::kTombstone))
                return Status::Corruption("invalid prefix entry");
            if (shared != 0U) internal_key.user_key.assign(previous_key->user_key, 0U, shared);
            internal_key.user_key.append(fields.substr(0U, unshared));
            fields.remove_prefix(unshared);
        } else {
            std::uint32_t internal_key_size = 0U;
            if (!GetFixed32(&fields, &internal_key_size).ok() ||
                internal_key_size < kMinInternalKeyBytes || internal_key_size > kMaxInternalKeyBytes ||
                fields.size() < internal_key_size)
                return Status::Corruption("invalid data-block InternalKey length");
            status = DecodeInternalKey(fields.substr(0U, internal_key_size), &internal_key);
            if (!status.ok()) return status;
            fields.remove_prefix(internal_key_size);
            if (!GetFixed32(&fields, &value_size).ok())
                return Status::Corruption("truncated data-block value length");
        }
        if (value_size > kMaxValueBytes || fields.size() < value_size)
            return Status::Corruption("invalid data-block value length");
        if (internal_key.record_type == RecordType::kTombstone &&
            value_size != 0U) {
            return Status::Corruption(
                "data-block tombstone contains value bytes");
        }

        if (previous_key.has_value() &&
            (SameUserKeyAndSequence(*previous_key, internal_key) ||
             compare.Compare(*previous_key, internal_key) >= 0)) {
            return Status::Corruption(
                "data-block entries are not strictly increasing");
        }

        const std::size_t uncompressed_size = 21U + internal_key.user_key.size() + value_size;
        if (uncompressed_size > kMaxDataBlockBodyBytes - kDataBlockHeaderBytes - uncompressed_bytes)
            return Status::Corruption("decoded data block exceeds 66 MiB limit");
        uncompressed_bytes += uncompressed_size;
        entries.push_back(DataBlockEntry{
            internal_key,
            std::string(fields.substr(0U, value_size)),
        });
        fields.remove_prefix(value_size);
        previous_key = std::move(internal_key);
    }
    if (!fields.empty()) {
        return Status::Corruption(
            "data-block has trailing bytes after its entries");
    }

    reader->entries_ = std::move(entries);
    reader->restart_entries_.clear();
    for (std::size_t i = 0; i < restarts.size(); ++i)
        reader->restart_entries_.push_back(i * kRestartInterval);
    return Status::Ok();
}

const std::vector<DataBlockEntry>& DataBlockReader::entries() const {
    return entries_;
}

std::size_t DataBlockReader::LowerBound(
    const InternalKey& target) const {
    const InternalKeyComparator compare;
    // Find the last restart <= target, then scan at most one restart group.
    if (!restart_entries_.empty()) {
        std::size_t low = 0, high = restart_entries_.size();
        while (low < high) {
            const std::size_t mid = low + (high - low) / 2U;
            if (compare.Compare(entries_[restart_entries_[mid]].internal_key, target) <= 0)
                low = mid + 1U;
            else high = mid;
        }
        std::size_t entry = restart_entries_[low == 0 ? 0 : low - 1U];
        while (entry < entries_.size() && compare(entries_[entry].internal_key, target)) ++entry;
        return entry;
    }
    std::size_t first = 0U;
    std::size_t count = entries_.size();
    while (count > 0U) {
        const std::size_t step = count / 2U;
        const std::size_t middle = first + step;
        if (compare(entries_[middle].internal_key, target)) {
            first = middle + 1U;
            count -= step + 1U;
        } else {
            count = step;
        }
    }
    return first;
}

std::size_t DataBlockReader::MemoryUsage() const {
    std::size_t bytes = sizeof(*this) + entries_.capacity() * sizeof(DataBlockEntry) +
        restart_entries_.capacity() * sizeof(std::size_t);
    for (const auto& entry : entries_)
        bytes += entry.internal_key.user_key.capacity() + entry.value.capacity();
    return bytes;
}

}  // namespace minilsm
