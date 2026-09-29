#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "common/coding.hpp"
#include "minilsm/format_limits.hpp"
#include "minilsm/status.hpp"
#include "test_harness.hpp"

namespace {

bool TestFixedWidthRoundTripsAndBytes() {
    std::string encoded;
    minilsm::PutFixed16(&encoded, 0U);
    minilsm::PutFixed16(&encoded, std::numeric_limits<std::uint16_t>::max());
    minilsm::PutFixed32(&encoded, 0x78563412U);
    minilsm::PutFixed32(
        &encoded, std::numeric_limits<std::uint32_t>::max());
    minilsm::PutFixed64(&encoded, 0x0123456789ABCDEFULL);
    minilsm::PutFixed64(
        &encoded, std::numeric_limits<std::uint64_t>::max());

    MINILSM_CHECK(encoded.size() == 28U);
    MINILSM_CHECK(
        encoded.substr(4U, 4U) ==
        std::string("\x12\x34\x56\x78", 4U));
    MINILSM_CHECK(
        encoded.substr(12U, 8U) ==
        std::string("\xEF\xCD\xAB\x89\x67\x45\x23\x01", 8U));

    std::string_view input = encoded;
    std::uint16_t value16 = 1U;
    std::uint32_t value32 = 1U;
    std::uint64_t value64 = 1U;
    MINILSM_CHECK(minilsm::GetFixed16(&input, &value16).ok());
    MINILSM_CHECK(value16 == 0U);
    MINILSM_CHECK(minilsm::GetFixed16(&input, &value16).ok());
    MINILSM_CHECK(value16 == std::numeric_limits<std::uint16_t>::max());
    MINILSM_CHECK(minilsm::GetFixed32(&input, &value32).ok());
    MINILSM_CHECK(value32 == 0x78563412U);
    MINILSM_CHECK(minilsm::GetFixed32(&input, &value32).ok());
    MINILSM_CHECK(value32 == std::numeric_limits<std::uint32_t>::max());
    MINILSM_CHECK(minilsm::GetFixed64(&input, &value64).ok());
    MINILSM_CHECK(value64 == 0x0123456789ABCDEFULL);
    MINILSM_CHECK(minilsm::GetFixed64(&input, &value64).ok());
    MINILSM_CHECK(value64 == std::numeric_limits<std::uint64_t>::max());
    MINILSM_CHECK(input.empty());
    return true;
}

bool TestFixedWidthTruncation() {
    const std::string bytes(8U, '\x7F');

    for (std::size_t size = 0; size < 2U; ++size) {
        std::string_view input(bytes.data(), size);
        const std::string_view original = input;
        std::uint16_t value = 9U;
        const minilsm::Status status = minilsm::GetFixed16(&input, &value);
        MINILSM_CHECK(
            status.code() == minilsm::Status::Code::kCorruption);
        MINILSM_CHECK(input == original);
        MINILSM_CHECK(value == 9U);
    }
    for (std::size_t size = 0; size < 4U; ++size) {
        std::string_view input(bytes.data(), size);
        const std::string_view original = input;
        std::uint32_t value = 9U;
        MINILSM_CHECK(
            minilsm::GetFixed32(&input, &value).code() ==
            minilsm::Status::Code::kCorruption);
        MINILSM_CHECK(input == original);
        MINILSM_CHECK(value == 9U);
    }
    for (std::size_t size = 0; size < 8U; ++size) {
        std::string_view input(bytes.data(), size);
        const std::string_view original = input;
        std::uint64_t value = 9U;
        MINILSM_CHECK(
            minilsm::GetFixed64(&input, &value).code() ==
            minilsm::Status::Code::kCorruption);
        MINILSM_CHECK(input == original);
        MINILSM_CHECK(value == 9U);
    }
    return true;
}

bool TestLengthPrefixedBytes() {
    std::string encoded;
    MINILSM_CHECK(
        minilsm::PutLengthPrefixedBytes("", 4U, &encoded).ok());
    MINILSM_CHECK(
        minilsm::PutLengthPrefixedBytes(
            std::string_view("a\0b", 3U), 4U, &encoded)
            .ok());

    std::string_view input = encoded;
    std::string value = "unchanged";
    MINILSM_CHECK(
        minilsm::GetLengthPrefixedBytes(&input, 4U, &value).ok());
    MINILSM_CHECK(value.empty());
    MINILSM_CHECK(
        minilsm::GetLengthPrefixedBytes(&input, 4U, &value).ok());
    MINILSM_CHECK(value == std::string("a\0b", 3U));
    MINILSM_CHECK(input.empty());

    std::string too_large(
        static_cast<std::size_t>(minilsm::kMaxUserKeyBytes) + 1U, 'x');
    encoded.clear();
    const std::string_view maximum_length_value(
        too_large.data(), minilsm::kMaxUserKeyBytes);
    MINILSM_CHECK(
        minilsm::PutLengthPrefixedBytes(
            maximum_length_value,
            minilsm::kMaxUserKeyBytes,
            &encoded)
            .ok());
    input = encoded;
    MINILSM_CHECK(
        minilsm::GetLengthPrefixedBytes(
            &input, minilsm::kMaxUserKeyBytes, &value)
            .ok());
    MINILSM_CHECK(value == maximum_length_value);
    MINILSM_CHECK(input.empty());

    encoded.clear();
    MINILSM_CHECK(
        minilsm::PutLengthPrefixedBytes(
            too_large, minilsm::kMaxUserKeyBytes, &encoded)
            .code() == minilsm::Status::Code::kInvalidArgument);
    MINILSM_CHECK(encoded.empty());
    return true;
}

bool TestMalformedLengthPrefixedBytes() {
    for (std::size_t size = 0; size < 4U; ++size) {
        const std::string bytes(size, '\0');
        std::string_view input = bytes;
        const std::string_view original = input;
        std::string value = "unchanged";
        MINILSM_CHECK(
            minilsm::GetLengthPrefixedBytes(&input, 8U, &value).code() ==
            minilsm::Status::Code::kCorruption);
        MINILSM_CHECK(input == original);
        MINILSM_CHECK(value == "unchanged");
    }

    std::string declared_too_large;
    minilsm::PutFixed32(
        &declared_too_large, minilsm::kMaxUserKeyBytes + 1U);
    std::string_view input = declared_too_large;
    std::string value = "unchanged";
    MINILSM_CHECK(
        minilsm::GetLengthPrefixedBytes(
            &input, minilsm::kMaxUserKeyBytes, &value)
            .code() == minilsm::Status::Code::kCorruption);
    MINILSM_CHECK(input == declared_too_large);
    MINILSM_CHECK(value == "unchanged");

    std::string truncated;
    minilsm::PutFixed32(&truncated, 4U);
    truncated.append("abc");
    input = truncated;
    MINILSM_CHECK(
        minilsm::GetLengthPrefixedBytes(&input, 4U, &value).code() ==
        minilsm::Status::Code::kCorruption);
    MINILSM_CHECK(input == truncated);
    MINILSM_CHECK(value == "unchanged");

    std::string with_trailing;
    minilsm::PutFixed32(&with_trailing, 2U);
    with_trailing.append("xy!");
    input = with_trailing;
    MINILSM_CHECK(
        minilsm::GetLengthPrefixedBytes(&input, 2U, &value).ok());
    MINILSM_CHECK(value == "xy");
    MINILSM_CHECK(input == "!");
    return true;
}

}  // namespace

int main() {
    MINILSM_RUN(TestFixedWidthRoundTripsAndBytes);
    MINILSM_RUN(TestFixedWidthTruncation);
    MINILSM_RUN(TestLengthPrefixedBytes);
    MINILSM_RUN(TestMalformedLengthPrefixedBytes);
    return 0;
}
