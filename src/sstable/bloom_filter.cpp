#include "sstable/bloom_filter.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/checksum.hpp"
#include "common/coding.hpp"
#include "common/internal_key.hpp"
#include "minilsm/format_limits.hpp"

namespace minilsm {

namespace {

std::uint32_t HashUserKey(std::string_view user_key) {
    std::uint32_t hash = 2'166'136'261U;
    for (const char byte : user_key) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 16'777'619U;
    }
    return hash;
}

bool CalculateBitCount(
    std::uint64_t key_count,
    std::uint64_t* bit_count) {
    if (key_count == 0U ||
        key_count >
            (std::numeric_limits<std::uint64_t>::max() - 7U) /
                kBloomBitsPerKey) {
        return false;
    }

    std::uint64_t bits = key_count * kBloomBitsPerKey;
    if (bits < 64U) {
        bits = 64U;
    }
    bits = (bits + 7U) & ~std::uint64_t{7U};
    const std::uint64_t byte_count = bits / 8U;
    if (byte_count >
        kMaxBloomFilterBodyBytes - kBloomFilterHeaderBytes) {
        return false;
    }
    *bit_count = bits;
    return true;
}

void SetProbeBits(
    std::uint32_t hash,
    std::uint64_t bit_count,
    std::string* bits) {
    const std::uint32_t delta =
        (hash >> 17U) | (hash << 15U);
    for (std::uint32_t probe = 0U; probe < kBloomHashCount; ++probe) {
        const std::uint64_t bit = hash % bit_count;
        const std::size_t byte_index =
            static_cast<std::size_t>(bit / 8U);
        const unsigned char mask =
            static_cast<unsigned char>(1U << (bit % 8U));
        const unsigned char current =
            static_cast<unsigned char>((*bits)[byte_index]);
        (*bits)[byte_index] = static_cast<char>(current | mask);
        hash += delta;
    }
}

bool AllProbeBitsSet(
    std::uint32_t hash,
    std::uint64_t bit_count,
    std::string_view bits) {
    const std::uint32_t delta =
        (hash >> 17U) | (hash << 15U);
    for (std::uint32_t probe = 0U; probe < kBloomHashCount; ++probe) {
        const std::uint64_t bit = hash % bit_count;
        const std::size_t byte_index =
            static_cast<std::size_t>(bit / 8U);
        const unsigned char mask =
            static_cast<unsigned char>(1U << (bit % 8U));
        if ((static_cast<unsigned char>(bits[byte_index]) & mask) == 0U) {
            return false;
        }
        hash += delta;
    }
    return true;
}

}  // namespace

Status BloomFilter::Build(
    const std::vector<std::string>& user_keys,
    BloomFilter* filter) {
    if (filter == nullptr) {
        return Status::InvalidArgument(
            "Bloom-filter build received a null output");
    }
    if (user_keys.empty()) {
        return Status::InvalidArgument(
            "a persisted Bloom filter requires at least one user key");
    }

    std::vector<std::string_view> distinct_keys;
    distinct_keys.reserve(user_keys.size());
    for (const std::string& user_key : user_keys) {
        if (user_key.size() > kMaxUserKeyBytes) {
            return Status::InvalidArgument(
                "Bloom-filter user key exceeds 1 MiB");
        }
        distinct_keys.push_back(user_key);
    }
    std::sort(
        distinct_keys.begin(),
        distinct_keys.end(),
        [](std::string_view left, std::string_view right) {
            return CompareUserKeys(left, right) < 0;
        });
    distinct_keys.erase(
        std::unique(distinct_keys.begin(), distinct_keys.end()),
        distinct_keys.end());

    if (distinct_keys.size() >
        std::numeric_limits<std::uint64_t>::max()) {
        return Status::InvalidArgument(
            "Bloom-filter key count exceeds uint64");
    }
    const std::uint64_t key_count = distinct_keys.size();
    std::uint64_t bit_count = 0U;
    if (!CalculateBitCount(key_count, &bit_count) ||
        bit_count / 8U > std::numeric_limits<std::size_t>::max()) {
        return Status::InvalidArgument(
            "Bloom-filter size exceeds version 1 bounds");
    }

    std::string bits(static_cast<std::size_t>(bit_count / 8U), '\0');
    for (const std::string_view user_key : distinct_keys) {
        SetProbeBits(HashUserKey(user_key), bit_count, &bits);
    }

    BloomFilter built;
    built.bit_count_ = bit_count;
    built.key_count_ = key_count;
    built.bits_ = std::move(bits);
    *filter = std::move(built);
    return Status::Ok();
}

Status BloomFilter::Encode(std::string* physical_block) const {
    if (physical_block == nullptr) {
        return Status::InvalidArgument(
            "Bloom-filter encode received a null output");
    }
    std::uint64_t expected_bit_count = 0U;
    if (!CalculateBitCount(key_count_, &expected_bit_count) ||
        bit_count_ != expected_bit_count ||
        bit_count_ / 8U != bits_.size()) {
        return Status::InvalidArgument(
            "Bloom-filter state is not a valid version 1 filter");
    }

    std::string body(kBloomFilterMagic);
    PutFixed16(&body, kBloomFilterFormatVersion);
    PutFixed16(&body, 0U);
    PutFixed32(&body, kBloomBitsPerKey);
    PutFixed32(&body, kBloomHashCount);
    PutFixed64(&body, bit_count_);
    PutFixed64(&body, key_count_);
    body.append(bits_);
    if (body.size() < kMinBloomFilterBodyBytes ||
        body.size() > kMaxBloomFilterBodyBytes) {
        return Status::InvalidArgument(
            "Bloom-filter body is outside version 1 bounds");
    }

    std::string result;
    result.reserve(body.size() + 8U);
    PutFixed32(&result, static_cast<std::uint32_t>(body.size()));
    result.append(body);
    PutFixed32(&result, Crc32c(result));
    *physical_block = std::move(result);
    return Status::Ok();
}

Status BloomFilter::Decode(
    std::string_view physical_block,
    BloomFilter* filter) {
    if (filter == nullptr) {
        return Status::InvalidArgument(
            "Bloom-filter decode received a null output");
    }
    if (physical_block.size() < kMinBloomFilterPhysicalBytes ||
        physical_block.size() > kMaxBloomFilterPhysicalBytes) {
        return Status::Corruption(
            "Bloom-filter physical size is outside version 1 bounds");
    }

    std::string_view size_input = physical_block;
    std::uint32_t body_size = 0U;
    Status status = GetFixed32(&size_input, &body_size);
    if (!status.ok() || body_size < kMinBloomFilterBodyBytes ||
        body_size > kMaxBloomFilterBodyBytes ||
        8U + static_cast<std::uint64_t>(body_size) !=
            physical_block.size()) {
        return Status::Corruption(
            "Bloom-filter body size is invalid");
    }

    std::string_view checksum_field =
        physical_block.substr(physical_block.size() - 4U);
    std::uint32_t stored_checksum = 0U;
    status = GetFixed32(&checksum_field, &stored_checksum);
    if (!status.ok() ||
        !VerifyCrc32c(
            physical_block.substr(0U, physical_block.size() - 4U),
            stored_checksum)) {
        return Status::Corruption("Bloom-filter checksum mismatch");
    }

    std::string_view body = physical_block.substr(4U, body_size);
    if (body.substr(0U, kBloomFilterMagic.size()) !=
        kBloomFilterMagic) {
        return Status::Corruption("Bloom-filter magic is invalid");
    }
    std::string_view fields = body.substr(kBloomFilterMagic.size());
    std::uint16_t version = 0U;
    std::uint16_t reserved = 0U;
    std::uint32_t bits_per_key = 0U;
    std::uint32_t hash_count = 0U;
    std::uint64_t bit_count = 0U;
    std::uint64_t key_count = 0U;
    status = GetFixed16(&fields, &version);
    if (status.ok()) {
        status = GetFixed16(&fields, &reserved);
    }
    if (status.ok()) {
        status = GetFixed32(&fields, &bits_per_key);
    }
    if (status.ok()) {
        status = GetFixed32(&fields, &hash_count);
    }
    if (status.ok()) {
        status = GetFixed64(&fields, &bit_count);
    }
    if (status.ok()) {
        status = GetFixed64(&fields, &key_count);
    }
    if (!status.ok() || version != kBloomFilterFormatVersion ||
        reserved != 0U || bits_per_key != kBloomBitsPerKey ||
        hash_count != kBloomHashCount) {
        return Status::Corruption(
            "Bloom-filter header fields are invalid");
    }

    std::uint64_t expected_bit_count = 0U;
    if (!CalculateBitCount(key_count, &expected_bit_count) ||
        bit_count != expected_bit_count || bit_count % 8U != 0U ||
        bit_count / 8U != fields.size()) {
        return Status::Corruption(
            "Bloom-filter bit and key lengths are inconsistent");
    }

    BloomFilter decoded;
    decoded.bit_count_ = bit_count;
    decoded.key_count_ = key_count;
    decoded.bits_.assign(fields);
    *filter = std::move(decoded);
    return Status::Ok();
}

bool BloomFilter::MayContain(std::string_view user_key) const {
    if (empty()) {
        return true;
    }
    return AllProbeBitsSet(
        HashUserKey(user_key), bit_count_, bits_);
}

bool BloomFilter::empty() const {
    return bit_count_ == 0U;
}

std::uint64_t BloomFilter::bit_count() const {
    return bit_count_;
}

std::uint64_t BloomFilter::key_count() const {
    return key_count_;
}

}  // namespace minilsm
