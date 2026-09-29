#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "minilsm/status.hpp"

namespace minilsm {

enum class FileKind {
    kWal,
    kSstable,
    kManifest,
    kCurrent,
};

struct ParsedFileName {
    FileKind kind = FileKind::kCurrent;
    std::uint64_t file_number = 0U;
    bool temporary = false;
};

Status MakeWalFileName(
    std::uint64_t file_number,
    bool temporary,
    std::string* name);
Status MakeSstableFileName(
    std::uint64_t file_number,
    bool temporary,
    std::string* name);
Status MakeManifestFileName(
    std::uint64_t file_number,
    bool temporary,
    std::string* name);

[[nodiscard]] std::string CurrentFileName(bool temporary);

// Accepts only the exact version 1 database basenames documented in
// FILE_FORMATS.md. Numbered names contain exactly 20 decimal digits and the
// parsed number must be in [1, UINT64_MAX].
Status ParseFileName(
    std::string_view name,
    ParsedFileName* parsed);

}  // namespace minilsm
