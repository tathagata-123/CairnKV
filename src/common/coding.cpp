#include "common/coding.hpp"

#include <cassert>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>

namespace minilsm {

namespace {

Status CheckDecodeArguments(
    const std::string_view* input,
    const void* value,
    std::string_view field_name) {
    if (input == nullptr || value == nullptr) {
        return Status::InvalidArgument(
            std::string(field_name) + " decoder received a null output");
    }
    return Status::Ok();
}

Status CheckRemaining(
    std::string_view input,
    std::size_t required,
    std::string_view field_name) {
    if (input.size() < required) {
        return Status::Corruption(
            "truncated " + std::string(field_name) + ": need " +
            std::to_string(required) + " bytes, have " +
            std::to_string(input.size()));
    }
    return Status::Ok();
}

}  // namespace

void PutFixed16(std::string* output, std::uint16_t value) {
    assert(output != nullptr);
    output->push_back(static_cast<char>(value & 0xFFU));
    output->push_back(static_cast<char>((value >> 8U) & 0xFFU));
}

void PutFixed32(std::string* output, std::uint32_t value) {
    assert(output != nullptr);
    for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
        output->push_back(
            static_cast<char>((value >> shift) & 0xFFU));
    }
}

void PutFixed64(std::string* output, std::uint64_t value) {
    assert(output != nullptr);
    for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
        output->push_back(
            static_cast<char>((value >> shift) & 0xFFU));
    }
}

Status GetFixed16(std::string_view* input, std::uint16_t* value) {
    Status status = CheckDecodeArguments(input, value, "u16");
    if (!status.ok()) {
        return status;
    }
    status = CheckRemaining(*input, 2U, "u16");
    if (!status.ok()) {
        return status;
    }

    const auto* bytes =
        reinterpret_cast<const unsigned char*>(input->data());
    const std::uint16_t decoded =
        static_cast<std::uint16_t>(bytes[0]) |
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[1]) << 8U);
    input->remove_prefix(2U);
    *value = decoded;
    return Status::Ok();
}

Status GetFixed32(std::string_view* input, std::uint32_t* value) {
    Status status = CheckDecodeArguments(input, value, "u32");
    if (!status.ok()) {
        return status;
    }
    status = CheckRemaining(*input, 4U, "u32");
    if (!status.ok()) {
        return status;
    }

    const auto* bytes =
        reinterpret_cast<const unsigned char*>(input->data());
    std::uint32_t decoded = 0U;
    for (unsigned int index = 0U; index < 4U; ++index) {
        decoded |= static_cast<std::uint32_t>(bytes[index])
                   << (index * 8U);
    }
    input->remove_prefix(4U);
    *value = decoded;
    return Status::Ok();
}

Status GetFixed64(std::string_view* input, std::uint64_t* value) {
    Status status = CheckDecodeArguments(input, value, "u64");
    if (!status.ok()) {
        return status;
    }
    status = CheckRemaining(*input, 8U, "u64");
    if (!status.ok()) {
        return status;
    }

    const auto* bytes =
        reinterpret_cast<const unsigned char*>(input->data());
    std::uint64_t decoded = 0U;
    for (unsigned int index = 0U; index < 8U; ++index) {
        decoded |= static_cast<std::uint64_t>(bytes[index])
                   << (index * 8U);
    }
    input->remove_prefix(8U);
    *value = decoded;
    return Status::Ok();
}

Status PutLengthPrefixedBytes(
    std::string_view value,
    std::uint32_t maximum_length,
    std::string* output) {
    if (output == nullptr) {
        return Status::InvalidArgument(
            "length-prefixed encoder received a null output");
    }
    if (value.size() > maximum_length ||
        value.size() > std::numeric_limits<std::uint32_t>::max()) {
        return Status::InvalidArgument(
            "byte-string length " + std::to_string(value.size()) +
            " exceeds maximum " + std::to_string(maximum_length));
    }

    PutFixed32(output, static_cast<std::uint32_t>(value.size()));
    output->append(value);
    return Status::Ok();
}

Status GetLengthPrefixedBytes(
    std::string_view* input,
    std::uint32_t maximum_length,
    std::string* value) {
    if (input == nullptr || value == nullptr) {
        return Status::InvalidArgument(
            "length-prefixed decoder received a null output");
    }

    std::string_view remaining = *input;
    std::uint32_t length = 0U;
    Status status = GetFixed32(&remaining, &length);
    if (!status.ok()) {
        return status;
    }
    if (length > maximum_length) {
        return Status::Corruption(
            "byte-string length " + std::to_string(length) +
            " exceeds maximum " + std::to_string(maximum_length));
    }
    status = CheckRemaining(remaining, length, "length-prefixed bytes");
    if (!status.ok()) {
        return status;
    }

    std::string decoded(remaining.substr(0U, length));
    remaining.remove_prefix(length);
    *value = std::move(decoded);
    *input = remaining;
    return Status::Ok();
}

}  // namespace minilsm
