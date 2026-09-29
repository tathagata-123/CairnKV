#include "sstable/sstable_format.hpp"

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
#include "sstable/bloom_filter.hpp"

namespace minilsm {

namespace {

bool SameUserKeyAndSequence(
    const InternalKey& left,
    const InternalKey& right) {
    return left.user_key == right.user_key &&
           left.sequence_number == right.sequence_number;
}

Status ValidateIndexKeyOrder(
    const std::optional<InternalKey>& previous,
    const InternalKey& current) {
    if (!previous.has_value()) {
        return Status::Ok();
    }
    const InternalKeyComparator compare;
    if (SameUserKeyAndSequence(*previous, current) ||
        compare.Compare(*previous, current) >= 0) {
        return Status::Corruption(
            "index largest keys are not strictly increasing");
    }
    return Status::Ok();
}

Status ValidateFramedIndexBlock(
    std::string_view physical_block,
    std::uint32_t* body_size) {
    if (physical_block.size() < kMinIndexBlockPhysicalBytes ||
        physical_block.size() > kMaxIndexBlockPhysicalBytes) {
        return Status::Corruption(
            "index-block physical size is outside version 1 bounds");
    }

    std::string_view input = physical_block;
    Status status = GetFixed32(&input, body_size);
    if (!status.ok() || *body_size < kMinIndexBlockBodyBytes ||
        *body_size > kMaxIndexBlockBodyBytes ||
        8U + static_cast<std::uint64_t>(*body_size) !=
            physical_block.size()) {
        return Status::Corruption(
            "index-block body size is invalid");
    }

    std::string_view checksum_input =
        physical_block.substr(0U, physical_block.size() - 4U);
    std::string_view checksum_field =
        physical_block.substr(physical_block.size() - 4U);
    std::uint32_t stored_checksum = 0U;
    status = GetFixed32(&checksum_field, &stored_checksum);
    if (!status.ok() ||
        !VerifyCrc32c(checksum_input, stored_checksum)) {
        return Status::Corruption("index-block checksum mismatch");
    }
    return Status::Ok();
}

Status ValidateFooterRegionSizes(const SstableFooter& footer) {
    if (footer.index_offset < kMinDataBlockPhysicalBytes ||
        footer.index_size < kMinIndexBlockPhysicalBytes ||
        footer.index_size > kMaxIndexBlockPhysicalBytes ||
        footer.entry_count == 0U) {
        return Status::Corruption(
            "SSTable footer index fields are outside version 1 bounds");
    }

    const bool filter_absent =
        footer.filter_offset == 0U && footer.filter_size == 0U;
    const bool filter_present =
        footer.filter_offset != 0U && footer.filter_size != 0U;
    if (!filter_absent && !filter_present) {
        return Status::Corruption(
            "SSTable footer filter fields have an invalid empty state");
    }
    if (filter_present &&
        (footer.filter_size < kMinBloomFilterPhysicalBytes ||
         footer.filter_size > kMaxBloomFilterPhysicalBytes)) {
        return Status::Corruption(
            "SSTable footer filter size is outside version 1 bounds");
    }
    return Status::Ok();
}

}  // namespace

Status EncodeIndexBlock(
    const std::vector<BlockIndexEntry>& entries,
    std::string* physical_block) {
    if (physical_block == nullptr) {
        return Status::InvalidArgument(
            "index-block encode received a null output");
    }
    if (entries.empty() ||
        entries.size() > std::numeric_limits<std::uint32_t>::max()) {
        return Status::InvalidArgument(
            "index block must contain a uint32 count of entries");
    }

    std::string body(kIndexBlockMagic);
    PutFixed16(&body, kSstableFormatVersion);
    PutFixed16(&body, 0U);
    PutFixed32(&body, static_cast<std::uint32_t>(entries.size()));

    std::optional<InternalKey> previous_key;
    std::uint64_t expected_offset = 0U;
    for (const BlockIndexEntry& entry : entries) {
        Status status =
            ValidateIndexKeyOrder(previous_key, entry.largest_internal_key);
        if (!status.ok()) {
            return Status::InvalidArgument(status.message());
        }
        if (entry.block_size < kMinDataBlockPhysicalBytes ||
            entry.block_size > kMaxDataBlockPhysicalBytes ||
            entry.block_offset != expected_offset ||
            entry.block_size >
                std::numeric_limits<std::uint64_t>::max() -
                    expected_offset) {
            return Status::InvalidArgument(
                "index block contains an invalid data-block handle");
        }

        std::string encoded_key;
        status = EncodeInternalKey(entry.largest_internal_key, &encoded_key);
        if (!status.ok()) {
            return status;
        }
        const std::size_t entry_size =
            4U + encoded_key.size() + 8U + 4U;
        if (body.size() > kMaxIndexBlockBodyBytes ||
            entry_size > kMaxIndexBlockBodyBytes - body.size()) {
            return Status::InvalidArgument(
                "index-block body exceeds 64 MiB");
        }

        PutFixed32(
            &body, static_cast<std::uint32_t>(encoded_key.size()));
        body.append(encoded_key);
        PutFixed64(&body, entry.block_offset);
        PutFixed32(&body, entry.block_size);
        expected_offset += entry.block_size;
        previous_key = entry.largest_internal_key;
    }

    std::string result;
    result.reserve(body.size() + 8U);
    PutFixed32(&result, static_cast<std::uint32_t>(body.size()));
    result.append(body);
    PutFixed32(&result, Crc32c(result));
    *physical_block = std::move(result);
    return Status::Ok();
}

Status DecodeIndexBlock(
    std::string_view physical_block,
    std::uint64_t data_region_size,
    std::vector<BlockIndexEntry>* entries) {
    if (entries == nullptr) {
        return Status::InvalidArgument(
            "index-block decode received a null output");
    }

    std::uint32_t body_size = 0U;
    Status status = ValidateFramedIndexBlock(physical_block, &body_size);
    if (!status.ok()) {
        return status;
    }
    std::string_view body = physical_block.substr(4U, body_size);
    if (body.substr(0U, kIndexBlockMagic.size()) != kIndexBlockMagic) {
        return Status::Corruption("index-block magic is invalid");
    }

    std::string_view fields = body.substr(kIndexBlockMagic.size());
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
    if (!status.ok() || version != kSstableFormatVersion ||
        reserved != 0U || entry_count == 0U) {
        return Status::Corruption(
            "index-block header fields are invalid");
    }
    if (entry_count > fields.size() / kMinIndexEntryBytes) {
        return Status::Corruption(
            "index-block entry count exceeds its bounded body");
    }

    std::vector<BlockIndexEntry> decoded;
    decoded.reserve(entry_count);
    std::optional<InternalKey> previous_key;
    std::uint64_t expected_offset = 0U;
    for (std::uint32_t index = 0U; index < entry_count; ++index) {
        std::uint32_t key_size = 0U;
        status = GetFixed32(&fields, &key_size);
        if (!status.ok() || key_size < kMinInternalKeyBytes ||
            key_size > kMaxInternalKeyBytes ||
            fields.size() < key_size) {
            return Status::Corruption(
                "index entry has an invalid largest-key length");
        }

        InternalKey largest_key;
        status = DecodeInternalKey(
            fields.substr(0U, key_size), &largest_key);
        if (!status.ok()) {
            return Status::Corruption(
                "index entry contains an invalid InternalKey: " +
                status.message());
        }
        fields.remove_prefix(key_size);

        std::uint64_t block_offset = 0U;
        std::uint32_t block_size = 0U;
        status = GetFixed64(&fields, &block_offset);
        if (status.ok()) {
            status = GetFixed32(&fields, &block_size);
        }
        if (!status.ok() ||
            block_size < kMinDataBlockPhysicalBytes ||
            block_size > kMaxDataBlockPhysicalBytes) {
            return Status::Corruption(
                "index entry has an invalid data-block size");
        }
        status = ValidateIndexKeyOrder(previous_key, largest_key);
        if (!status.ok()) {
            return status;
        }
        if (block_offset != expected_offset ||
            block_offset > data_region_size ||
            block_size > data_region_size - block_offset) {
            return Status::Corruption(
                "index entry data-block handle is outside the data region");
        }

        decoded.push_back(BlockIndexEntry{
            largest_key, block_offset, block_size});
        expected_offset = block_offset + block_size;
        previous_key = std::move(largest_key);
    }
    if (!fields.empty()) {
        return Status::Corruption(
            "index block has trailing bytes after its entries");
    }
    if (expected_offset != data_region_size) {
        return Status::Corruption(
            "index data-block handles do not fill the data region");
    }

    *entries = std::move(decoded);
    return Status::Ok();
}

Status EncodeSstableFooter(
    const SstableFooter& footer,
    std::string* encoded) {
    if (encoded == nullptr) {
        return Status::InvalidArgument(
            "SSTable footer encode received a null output");
    }
    Status status = ValidateFooterRegionSizes(footer);
    if (!status.ok()) {
        return Status::InvalidArgument(status.message());
    }

    if (footer.index_offset > kMaxDatabaseFileBytes ||
        footer.index_size > kMaxDatabaseFileBytes - footer.index_offset) {
        return Status::InvalidArgument(
            "SSTable index end exceeds the file-size bound");
    }
    const std::uint64_t index_end =
        footer.index_offset + footer.index_size;
    std::uint64_t content_end = index_end;
    if (footer.filter_offset != 0U) {
        if (footer.filter_offset != index_end ||
            footer.filter_size >
                kMaxDatabaseFileBytes - footer.filter_offset) {
            return Status::InvalidArgument(
                "SSTable filter does not immediately follow the index");
        }
        content_end = footer.filter_offset + footer.filter_size;
    }
    if (content_end >
        kMaxDatabaseFileBytes - kSstableFooterBytes) {
        return Status::InvalidArgument(
            "SSTable footer would exceed the file-size bound");
    }

    std::string result(kSstableMagic);
    PutFixed32(&result, kSstableFormatVersion);
    PutFixed32(
        &result, static_cast<std::uint32_t>(kSstableFooterBytes));
    PutFixed64(&result, footer.filter_offset);
    PutFixed64(&result, footer.filter_size);
    PutFixed64(&result, footer.index_offset);
    PutFixed64(&result, footer.index_size);
    PutFixed64(&result, footer.entry_count);
    PutFixed32(&result, 0U);
    PutFixed32(&result, Crc32c(result));
    *encoded = std::move(result);
    return Status::Ok();
}

Status DecodeSstableFooter(
    std::string_view encoded,
    SstableFooter* footer) {
    if (footer == nullptr) {
        return Status::InvalidArgument(
            "SSTable footer decode received a null output");
    }
    if (encoded.size() != kSstableFooterBytes) {
        return Status::Corruption(
            "SSTable footer is not exactly 64 bytes");
    }
    if (encoded.substr(0U, kSstableMagic.size()) != kSstableMagic) {
        return Status::Corruption("SSTable footer magic is invalid");
    }

    std::string_view fields = encoded.substr(kSstableMagic.size());
    std::uint32_t version = 0U;
    std::uint32_t footer_size = 0U;
    SstableFooter decoded;
    std::uint32_t reserved = 0U;
    std::uint32_t stored_checksum = 0U;
    Status status = GetFixed32(&fields, &version);
    if (status.ok()) {
        status = GetFixed32(&fields, &footer_size);
    }
    if (status.ok()) {
        status = GetFixed64(&fields, &decoded.filter_offset);
    }
    if (status.ok()) {
        status = GetFixed64(&fields, &decoded.filter_size);
    }
    if (status.ok()) {
        status = GetFixed64(&fields, &decoded.index_offset);
    }
    if (status.ok()) {
        status = GetFixed64(&fields, &decoded.index_size);
    }
    if (status.ok()) {
        status = GetFixed64(&fields, &decoded.entry_count);
    }
    if (status.ok()) {
        status = GetFixed32(&fields, &reserved);
    }
    if (status.ok()) {
        status = GetFixed32(&fields, &stored_checksum);
    }
    if (!status.ok() || !fields.empty()) {
        return Status::Corruption("SSTable footer fields are malformed");
    }
    if (version != kSstableFormatVersion) {
        return Status::Corruption(
            "SSTable footer has an unsupported version");
    }
    if (footer_size != kSstableFooterBytes || reserved != 0U) {
        return Status::Corruption(
            "SSTable footer size or reserved field is invalid");
    }
    if (!VerifyCrc32c(encoded.substr(0U, 60U), stored_checksum)) {
        return Status::Corruption("SSTable footer checksum mismatch");
    }
    status = ValidateFooterRegionSizes(decoded);
    if (!status.ok()) {
        return status;
    }

    *footer = decoded;
    return Status::Ok();
}

}  // namespace minilsm
