#include "common/checksum.hpp"

#include <cstdint>
#include <string_view>

namespace minilsm {

std::uint32_t Crc32c(std::string_view bytes) {
    constexpr std::uint32_t kReflectedPolynomial = 0x82F63B78U;
    std::uint32_t checksum = 0xFFFFFFFFU;

    for (const char byte : bytes) {
        checksum ^= static_cast<unsigned char>(byte);
        for (unsigned int bit = 0U; bit < 8U; ++bit) {
            if ((checksum & 1U) != 0U) {
                checksum =
                    (checksum >> 1U) ^ kReflectedPolynomial;
            } else {
                checksum >>= 1U;
            }
        }
    }
    return checksum ^ 0xFFFFFFFFU;
}

bool VerifyCrc32c(
    std::string_view bytes,
    std::uint32_t expected_checksum) {
    return Crc32c(bytes) == expected_checksum;
}

}  // namespace minilsm
