#include <cstdint>
#include <string>
#include <string_view>

#include "common/checksum.hpp"
#include "test_harness.hpp"

namespace {

bool TestKnownCrc32cValues() {
    MINILSM_CHECK(minilsm::Crc32c("") == 0U);
    MINILSM_CHECK(minilsm::Crc32c("123456789") == 0xE3069283U);
    MINILSM_CHECK(minilsm::Crc32c("abc") == 0x364B3FB7U);

    const std::string binary("\0\x80\xFF\x01", 4U);
    const std::uint32_t checksum = minilsm::Crc32c(binary);
    MINILSM_CHECK(checksum == minilsm::Crc32c(binary));
    MINILSM_CHECK(minilsm::VerifyCrc32c(binary, checksum));
    return true;
}

bool TestSingleByteCorruptionIsDetected() {
    std::string bytes = "deterministic checksum input";
    const std::uint32_t checksum = minilsm::Crc32c(bytes);
    MINILSM_CHECK(minilsm::VerifyCrc32c(bytes, checksum));

    bytes[7] = static_cast<char>(
        static_cast<unsigned char>(bytes[7]) ^ 0x01U);
    MINILSM_CHECK(!minilsm::VerifyCrc32c(bytes, checksum));
    MINILSM_CHECK(!minilsm::VerifyCrc32c(
        std::string_view("deterministic checksum inpu"), checksum));
    return true;
}

}  // namespace

int main() {
    MINILSM_RUN(TestKnownCrc32cValues);
    MINILSM_RUN(TestSingleByteCorruptionIsDetected);
    return 0;
}
