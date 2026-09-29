#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "minilsm/status.hpp"

namespace minilsm {

// Fixed-width integers are appended least-significant byte first. Decoders
// consume input only after confirming that the complete field is present.
void PutFixed16(std::string* output, std::uint16_t value);
void PutFixed32(std::string* output, std::uint32_t value);
void PutFixed64(std::string* output, std::uint64_t value);

Status GetFixed16(std::string_view* input, std::uint16_t* value);
Status GetFixed32(std::string_view* input, std::uint32_t* value);
Status GetFixed64(std::string_view* input, std::uint64_t* value);

// A length-prefixed byte string is a little-endian u32 length followed by
// exactly that many bytes. The caller supplies the field-specific format
// bound (for example, kMaxUserKeyBytes or kMaxValueBytes).
Status PutLengthPrefixedBytes(
    std::string_view value,
    std::uint32_t maximum_length,
    std::string* output);
Status GetLengthPrefixedBytes(
    std::string_view* input,
    std::uint32_t maximum_length,
    std::string* value);

}  // namespace minilsm
