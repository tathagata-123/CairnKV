#include "common/internal_key.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "common/coding.hpp"
#include "minilsm/format_limits.hpp"

namespace minilsm {

namespace {

bool IsValidRecordType(RecordType record_type) {
    return record_type == RecordType::kTombstone ||
           record_type == RecordType::kValue;
}

std::string RecordTypeName(RecordType record_type) {
    if (record_type == RecordType::kTombstone) {
        return "TOMBSTONE";
    }
    if (record_type == RecordType::kValue) {
        return "VALUE";
    }
    return "INVALID";
}

}  // namespace

int CompareUserKeys(std::string_view left, std::string_view right) {
    const std::size_t common_size =
        left.size() < right.size() ? left.size() : right.size();
    for (std::size_t index = 0U; index < common_size; ++index) {
        const unsigned char left_byte =
            static_cast<unsigned char>(left[index]);
        const unsigned char right_byte =
            static_cast<unsigned char>(right[index]);
        if (left_byte < right_byte) {
            return -1;
        }
        if (left_byte > right_byte) {
            return 1;
        }
    }
    if (left.size() < right.size()) {
        return -1;
    }
    if (left.size() > right.size()) {
        return 1;
    }
    return 0;
}

int InternalKeyComparator::Compare(
    const InternalKey& left,
    const InternalKey& right) const {
    const int user_key_order =
        CompareUserKeys(left.user_key, right.user_key);
    if (user_key_order != 0) {
        return user_key_order;
    }
    if (left.sequence_number > right.sequence_number) {
        return -1;
    }
    if (left.sequence_number < right.sequence_number) {
        return 1;
    }

    const std::uint8_t left_type =
        static_cast<std::uint8_t>(left.record_type);
    const std::uint8_t right_type =
        static_cast<std::uint8_t>(right.record_type);
    if (left_type < right_type) {
        return -1;
    }
    if (left_type > right_type) {
        return 1;
    }
    return 0;
}

bool InternalKeyComparator::operator()(
    const InternalKey& left,
    const InternalKey& right) const {
    return Compare(left, right) < 0;
}

Status EncodeInternalKey(
    const InternalKey& key,
    std::string* encoded) {
    if (encoded == nullptr) {
        return Status::InvalidArgument(
            "InternalKey encoder received a null output");
    }
    if (key.user_key.size() > kMaxUserKeyBytes) {
        return Status::InvalidArgument(
            "InternalKey user key exceeds 1 MiB");
    }
    if (key.sequence_number == 0U) {
        return Status::InvalidArgument(
            "InternalKey sequence number must be nonzero");
    }
    if (!IsValidRecordType(key.record_type)) {
        return Status::InvalidArgument(
            "InternalKey has an invalid record type");
    }

    std::string result;
    result.reserve(kMinInternalKeyBytes + key.user_key.size());
    PutFixed32(
        &result, static_cast<std::uint32_t>(key.user_key.size()));
    result.append(key.user_key);
    PutFixed64(&result, key.sequence_number);
    result.push_back(static_cast<char>(key.record_type));
    *encoded = std::move(result);
    return Status::Ok();
}

Status DecodeInternalKey(
    std::string_view encoded,
    InternalKey* key) {
    if (key == nullptr) {
        return Status::InvalidArgument(
            "InternalKey decoder received a null output");
    }
    if (encoded.size() < kMinInternalKeyBytes) {
        return Status::Corruption(
            "encoded InternalKey is shorter than 13 bytes");
    }

    std::string_view remaining = encoded;
    std::uint32_t user_key_length = 0U;
    Status status = GetFixed32(&remaining, &user_key_length);
    if (!status.ok()) {
        return status;
    }
    if (user_key_length > kMaxUserKeyBytes) {
        return Status::Corruption(
            "InternalKey user-key length exceeds 1 MiB");
    }

    const std::size_t expected_size =
        kMinInternalKeyBytes +
        static_cast<std::size_t>(user_key_length);
    if (encoded.size() != expected_size) {
        return Status::Corruption(
            "encoded InternalKey size does not match its user-key length");
    }

    std::string user_key(remaining.substr(0U, user_key_length));
    remaining.remove_prefix(user_key_length);

    std::uint64_t sequence_number = 0U;
    status = GetFixed64(&remaining, &sequence_number);
    if (!status.ok()) {
        return status;
    }
    if (sequence_number == 0U) {
        return Status::Corruption(
            "InternalKey sequence number is zero");
    }

    const RecordType record_type =
        static_cast<RecordType>(
            static_cast<unsigned char>(remaining.front()));
    remaining.remove_prefix(1U);
    if (!IsValidRecordType(record_type)) {
        return Status::Corruption(
            "InternalKey record type is not TOMBSTONE or VALUE");
    }

    *key = InternalKey{
        std::move(user_key), sequence_number, record_type};
    return Status::Ok();
}

std::string InternalKeyDebugString(const InternalKey& key) {
    constexpr char kHexDigits[] = "0123456789abcdef";
    std::string key_hex;
    key_hex.reserve(key.user_key.size() * 2U);
    for (const char byte : key.user_key) {
        const unsigned char value =
            static_cast<unsigned char>(byte);
        key_hex.push_back(kHexDigits[value >> 4U]);
        key_hex.push_back(kHexDigits[value & 0x0FU]);
    }

    return "InternalKey{user_key=0x" + key_hex +
           ", sequence=" + std::to_string(key.sequence_number) +
           ", type=" + RecordTypeName(key.record_type) + "}";
}

}  // namespace minilsm
