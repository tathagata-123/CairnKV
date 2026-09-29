#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "manifest/version.hpp"
#include "minilsm/status.hpp"

namespace minilsm {

// Complete immutable MANIFEST snapshot version 1. The checksum covers every
// byte except the final stored u32 checksum field.
Status EncodeManifestSnapshot(
    const Version& version,
    std::string* encoded);
Status DecodeManifestSnapshot(
    std::string_view encoded,
    std::uint64_t expected_manifest_file_number,
    Version* version);

// CURRENT is exactly "MANIFEST-<20 digits>\n" (30 bytes). It is installed
// through CURRENT.tmp, fsync, atomic replacement, and directory fsync.
Status EncodeCurrent(
    std::uint64_t manifest_file_number,
    std::string* encoded);
Status DecodeCurrent(
    std::string_view encoded,
    std::uint64_t* manifest_file_number);

}  // namespace minilsm
