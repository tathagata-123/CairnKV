#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/internal_key.hpp"
#include "minilsm/status.hpp"

namespace minilsm {

inline constexpr std::string_view kDataBlockMagic = "MDB1";
inline constexpr std::string_view kIndexBlockMagic = "MIB1";
inline constexpr std::string_view kSstableMagic = "MLSMST01";
inline constexpr std::uint16_t kSstableFormatVersion = 1U;

inline constexpr std::size_t kDataBlockHeaderBytes = 12U;
inline constexpr std::size_t kMinDataBlockEntryBytes = 21U;
inline constexpr std::size_t kMinDataBlockBodyBytes = 33U;
inline constexpr std::size_t kMaxDataBlockBodyBytes = 69'206'016U;
inline constexpr std::size_t kMinDataBlockPhysicalBytes = 41U;
inline constexpr std::size_t kMaxDataBlockPhysicalBytes = 69'206'024U;

inline constexpr std::size_t kIndexBlockHeaderBytes = 12U;
inline constexpr std::size_t kMinIndexEntryBytes = 29U;
inline constexpr std::size_t kMinIndexBlockBodyBytes = 41U;
inline constexpr std::size_t kMaxIndexBlockBodyBytes = 67'108'864U;
inline constexpr std::size_t kMinIndexBlockPhysicalBytes = 49U;
inline constexpr std::size_t kMaxIndexBlockPhysicalBytes = 67'108'872U;

inline constexpr std::size_t kSstableFooterBytes = 64U;
inline constexpr std::uint64_t kMinSstableFileBytes = 154U;

struct BlockIndexEntry {
    InternalKey largest_internal_key;
    std::uint64_t block_offset = 0U;
    std::uint32_t block_size = 0U;
};

struct SstableFooter {
    std::uint64_t filter_offset = 0U;
    std::uint64_t filter_size = 0U;
    std::uint64_t index_offset = 0U;
    std::uint64_t index_size = 0U;
    std::uint64_t entry_count = 0U;
};

// Index blocks and the footer are persisted formats, not generic metadata
// serializers. These helpers implement only their fixed version-1 layouts.
Status EncodeIndexBlock(
    const std::vector<BlockIndexEntry>& entries,
    std::string* physical_block);
Status DecodeIndexBlock(
    std::string_view physical_block,
    std::uint64_t data_region_size,
    std::vector<BlockIndexEntry>* entries);

Status EncodeSstableFooter(
    const SstableFooter& footer,
    std::string* encoded);
Status DecodeSstableFooter(
    std::string_view encoded,
    SstableFooter* footer);

}  // namespace minilsm
