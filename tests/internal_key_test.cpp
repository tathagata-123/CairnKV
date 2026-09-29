#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/coding.hpp"
#include "common/internal_key.hpp"
#include "minilsm/format_limits.hpp"
#include "minilsm/status.hpp"
#include "test_harness.hpp"

namespace {

using minilsm::InternalKey;
using minilsm::InternalKeyComparator;
using minilsm::RecordType;

bool KeysEqual(const InternalKey& left, const InternalKey& right) {
    return left.user_key == right.user_key &&
           left.sequence_number == right.sequence_number &&
           left.record_type == right.record_type;
}

bool TestLogicalOrdering() {
    const InternalKeyComparator compare;
    MINILSM_CHECK(
        static_cast<std::uint8_t>(RecordType::kTombstone) == 0U);
    MINILSM_CHECK(
        static_cast<std::uint8_t>(RecordType::kValue) == 1U);

    MINILSM_CHECK(compare(
        InternalKey{"a", 1U, RecordType::kValue},
        InternalKey{"b", 99U, RecordType::kValue}));
    MINILSM_CHECK(compare(
        InternalKey{"a", 1U, RecordType::kValue},
        InternalKey{"aa", 99U, RecordType::kValue}));

    MINILSM_CHECK(compare(
        InternalKey{"key", 10U, RecordType::kValue},
        InternalKey{"key", 9U, RecordType::kValue}));
    MINILSM_CHECK(!compare(
        InternalKey{"key", 9U, RecordType::kValue},
        InternalKey{"key", 10U, RecordType::kValue}));

    MINILSM_CHECK(compare(
        InternalKey{"key", 10U, RecordType::kTombstone},
        InternalKey{"key", 10U, RecordType::kValue}));
    MINILSM_CHECK(!compare(
        InternalKey{"key", 10U, RecordType::kValue},
        InternalKey{"key", 10U, RecordType::kTombstone}));

    const std::string byte_7f(1U, '\x7F');
    const std::string byte_80(1U, static_cast<char>(0x80U));
    const std::string byte_ff(1U, static_cast<char>(0xFFU));
    MINILSM_CHECK(compare(
        InternalKey{byte_7f, 1U, RecordType::kValue},
        InternalKey{byte_80, 1U, RecordType::kValue}));
    MINILSM_CHECK(compare(
        InternalKey{byte_80, 1U, RecordType::kValue},
        InternalKey{byte_ff, 1U, RecordType::kValue}));

    const InternalKey same{"same", 1U, RecordType::kValue};
    MINILSM_CHECK(!compare(same, same));
    return true;
}

bool TestEncodingRoundTripAndExactLayout() {
    const InternalKey key{
        std::string("a\0\xFF", 3U),
        0x0102030405060708ULL,
        RecordType::kTombstone,
    };
    std::string encoded;
    MINILSM_CHECK(minilsm::EncodeInternalKey(key, &encoded).ok());
    MINILSM_CHECK(encoded.size() == 16U);
    MINILSM_CHECK(
        encoded.substr(0U, 4U) == std::string("\x03\0\0\0", 4U));
    MINILSM_CHECK(encoded.substr(4U, 3U) == key.user_key);
    MINILSM_CHECK(
        encoded.substr(7U, 8U) ==
        std::string("\x08\x07\x06\x05\x04\x03\x02\x01", 8U));
    MINILSM_CHECK(
        static_cast<unsigned char>(encoded.back()) == 0U);

    InternalKey decoded;
    MINILSM_CHECK(minilsm::DecodeInternalKey(encoded, &decoded).ok());
    MINILSM_CHECK(KeysEqual(key, decoded));

    const InternalKey maximum_sequence{
        "",
        std::numeric_limits<std::uint64_t>::max(),
        RecordType::kValue,
    };
    encoded.clear();
    MINILSM_CHECK(
        minilsm::EncodeInternalKey(maximum_sequence, &encoded).ok());
    MINILSM_CHECK(encoded.size() == minilsm::kMinInternalKeyBytes);
    MINILSM_CHECK(minilsm::DecodeInternalKey(encoded, &decoded).ok());
    MINILSM_CHECK(KeysEqual(maximum_sequence, decoded));

    const InternalKey maximum_user_key{
        std::string(minilsm::kMaxUserKeyBytes, 'k'),
        1U,
        RecordType::kValue,
    };
    encoded.clear();
    MINILSM_CHECK(
        minilsm::EncodeInternalKey(maximum_user_key, &encoded).ok());
    MINILSM_CHECK(encoded.size() == minilsm::kMaxInternalKeyBytes);
    MINILSM_CHECK(minilsm::DecodeInternalKey(encoded, &decoded).ok());
    MINILSM_CHECK(KeysEqual(maximum_user_key, decoded));
    return true;
}

bool TestMalformedInternalKeys() {
    InternalKey decoded{"unchanged", 7U, RecordType::kValue};
    for (std::size_t size = 0; size < minilsm::kMinInternalKeyBytes;
         ++size) {
        const std::string encoded(size, '\0');
        MINILSM_CHECK(
            minilsm::DecodeInternalKey(encoded, &decoded).code() ==
            minilsm::Status::Code::kCorruption);
        MINILSM_CHECK(decoded.user_key == "unchanged");
    }

    std::string invalid;
    minilsm::PutFixed32(&invalid, minilsm::kMaxUserKeyBytes + 1U);
    invalid.resize(minilsm::kMinInternalKeyBytes, '\0');
    MINILSM_CHECK(
        minilsm::DecodeInternalKey(invalid, &decoded).code() ==
        minilsm::Status::Code::kCorruption);

    invalid.clear();
    minilsm::PutFixed32(&invalid, 2U);
    invalid.append("x");
    minilsm::PutFixed64(&invalid, 1U);
    invalid.push_back(static_cast<char>(RecordType::kValue));
    MINILSM_CHECK(
        minilsm::DecodeInternalKey(invalid, &decoded).code() ==
        minilsm::Status::Code::kCorruption);

    const InternalKey valid{"key", 1U, RecordType::kValue};
    MINILSM_CHECK(minilsm::EncodeInternalKey(valid, &invalid).ok());
    invalid.push_back('!');
    MINILSM_CHECK(
        minilsm::DecodeInternalKey(invalid, &decoded).code() ==
        minilsm::Status::Code::kCorruption);

    InternalKey zero_sequence{"key", 0U, RecordType::kValue};
    invalid.clear();
    MINILSM_CHECK(
        minilsm::EncodeInternalKey(zero_sequence, &invalid).code() ==
        minilsm::Status::Code::kInvalidArgument);
    MINILSM_CHECK(invalid.empty());

    invalid.clear();
    minilsm::PutFixed32(&invalid, 0U);
    minilsm::PutFixed64(&invalid, 0U);
    invalid.push_back(static_cast<char>(RecordType::kValue));
    MINILSM_CHECK(
        minilsm::DecodeInternalKey(invalid, &decoded).code() ==
        minilsm::Status::Code::kCorruption);

    invalid.clear();
    minilsm::PutFixed32(&invalid, 0U);
    minilsm::PutFixed64(&invalid, 1U);
    invalid.push_back('\x02');
    MINILSM_CHECK(
        minilsm::DecodeInternalKey(invalid, &decoded).code() ==
        minilsm::Status::Code::kCorruption);

    InternalKey invalid_type{
        "key", 1U, static_cast<RecordType>(2U)};
    invalid.clear();
    MINILSM_CHECK(
        minilsm::EncodeInternalKey(invalid_type, &invalid).code() ==
        minilsm::Status::Code::kInvalidArgument);

    InternalKey oversized{
        std::string(
            static_cast<std::size_t>(minilsm::kMaxUserKeyBytes) + 1U,
            'x'),
        1U,
        RecordType::kValue,
    };
    MINILSM_CHECK(
        minilsm::EncodeInternalKey(oversized, &invalid).code() ==
        minilsm::Status::Code::kInvalidArgument);
    return true;
}

InternalKey RandomKey(std::mt19937_64* random) {
    const std::size_t length =
        static_cast<std::size_t>((*random)() % 33U);
    std::string user_key(length, '\0');
    for (char& byte : user_key) {
        byte = static_cast<char>((*random)() & 0xFFU);
    }
    std::uint64_t sequence_number = (*random)();
    if (sequence_number == 0U) {
        sequence_number = 1U;
    }
    const RecordType record_type =
        ((*random)() & 1U) == 0U
            ? RecordType::kTombstone
            : RecordType::kValue;
    return InternalKey{
        std::move(user_key), sequence_number, record_type};
}

bool TestDeterministicRandomizedOrdering() {
    constexpr std::uint64_t kSeed = 0x4D31494E544B4559ULL;
    std::mt19937_64 random(kSeed);
    const InternalKeyComparator compare;

    for (std::size_t iteration = 0; iteration < 5000U; ++iteration) {
        const InternalKey left = RandomKey(&random);
        const InternalKey right = RandomKey(&random);

        std::string left_encoded;
        std::string right_encoded;
        MINILSM_CHECK(
            minilsm::EncodeInternalKey(left, &left_encoded).ok());
        MINILSM_CHECK(
            minilsm::EncodeInternalKey(right, &right_encoded).ok());

        InternalKey left_decoded;
        InternalKey right_decoded;
        MINILSM_CHECK(
            minilsm::DecodeInternalKey(left_encoded, &left_decoded).ok());
        MINILSM_CHECK(
            minilsm::DecodeInternalKey(
                right_encoded, &right_decoded)
                .ok());
        MINILSM_CHECK(
            compare(left, right) ==
            compare(left_decoded, right_decoded));
        MINILSM_CHECK(
            compare(right, left) ==
            compare(right_decoded, left_decoded));
    }
    return true;
}

bool TestEncodedBytesAreNotTheComparator() {
    const InternalKey logically_first{
        "aa", 1U, RecordType::kValue};
    const InternalKey logically_second{
        "b", 1U, RecordType::kValue};
    const InternalKeyComparator compare;
    MINILSM_CHECK(compare(logically_first, logically_second));

    std::string first_encoded;
    std::string second_encoded;
    MINILSM_CHECK(
        minilsm::EncodeInternalKey(logically_first, &first_encoded).ok());
    MINILSM_CHECK(
        minilsm::EncodeInternalKey(logically_second, &second_encoded).ok());
    MINILSM_CHECK(first_encoded > second_encoded);
    return true;
}

bool TestDebugString() {
    const InternalKey key{
        std::string("A\0\xFF", 3U), 42U, RecordType::kValue};
    const std::string debug = minilsm::InternalKeyDebugString(key);
    MINILSM_CHECK(debug.find("4100ff") != std::string::npos);
    MINILSM_CHECK(debug.find("42") != std::string::npos);
    MINILSM_CHECK(debug.find("VALUE") != std::string::npos);
    return true;
}

}  // namespace

int main() {
    MINILSM_RUN(TestLogicalOrdering);
    MINILSM_RUN(TestEncodingRoundTripAndExactLayout);
    MINILSM_RUN(TestMalformedInternalKeys);
    MINILSM_RUN(TestDeterministicRandomizedOrdering);
    MINILSM_RUN(TestEncodedBytesAreNotTheComparator);
    MINILSM_RUN(TestDebugString);
    return 0;
}
