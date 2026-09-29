#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "minilsm/status.hpp"

namespace minilsm {

inline constexpr std::string_view kBloomFilterMagic = "MBF1";
inline constexpr std::uint16_t kBloomFilterFormatVersion = 1U;
inline constexpr std::uint32_t kBloomBitsPerKey = 10U;
inline constexpr std::uint32_t kBloomHashCount = 6U;
inline constexpr std::size_t kBloomFilterHeaderBytes = 32U;
inline constexpr std::size_t kMinBloomFilterBodyBytes = 40U;
inline constexpr std::size_t kMaxBloomFilterBodyBytes = 67'108'864U;
inline constexpr std::size_t kMinBloomFilterPhysicalBytes = 48U;
inline constexpr std::size_t kMaxBloomFilterPhysicalBytes = 67'108'872U;

// One concrete version-1 full-file Bloom filter. The default object models
// a compatible absent filter and therefore answers conservatively.
class BloomFilter {
public:
    static Status Build(
        const std::vector<std::string>& user_keys,
        BloomFilter* filter);
    static Status Decode(
        std::string_view physical_block,
        BloomFilter* filter);

    Status Encode(std::string* physical_block) const;

    // false proves absence; true means only possibly present. An absent
    // in-memory filter always returns true.
    [[nodiscard]] bool MayContain(std::string_view user_key) const;
    [[nodiscard]] bool empty() const;
    [[nodiscard]] std::uint64_t bit_count() const;
    [[nodiscard]] std::uint64_t key_count() const;

private:
    std::uint64_t bit_count_ = 0U;
    std::uint64_t key_count_ = 0U;
    std::string bits_;
};

}  // namespace minilsm
