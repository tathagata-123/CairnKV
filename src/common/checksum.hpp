#pragma once

#include <cstdint>
#include <string_view>

namespace minilsm {

// CRC-32C (Castagnoli), reflected polynomial 0x82F63B78, initial and
// final XOR 0xFFFFFFFF. Persisted checksum fields store this value directly.
[[nodiscard]] std::uint32_t Crc32c(std::string_view bytes);
[[nodiscard]] bool VerifyCrc32c(
    std::string_view bytes,
    std::uint32_t expected_checksum);

}  // namespace minilsm
