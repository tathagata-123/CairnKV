#include "common/file_names.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace minilsm {

namespace {

Status MakeNumberedName(
    std::string_view prefix,
    std::string_view final_suffix,
    std::uint64_t file_number,
    bool temporary,
    std::string* name) {
    if (name == nullptr) {
        return Status::InvalidArgument(
            "file-name creator received a null output");
    }
    if (file_number == 0U) {
        return Status::InvalidArgument(
            "CairnKV file number must be nonzero");
    }

    const std::string digits = std::to_string(file_number);
    std::string result(prefix);
    result.append(20U - digits.size(), '0');
    result.append(digits);
    result.append(final_suffix);
    if (temporary) {
        result.append(".tmp");
    }
    *name = std::move(result);
    return Status::Ok();
}

Status ParseFileNumber(
    std::string_view digits,
    std::uint64_t* file_number) {
    std::uint64_t result = 0U;
    for (const char byte : digits) {
        if (byte < '0' || byte > '9') {
            return Status::InvalidArgument(
                "file number contains a non-decimal byte");
        }
        const std::uint64_t digit =
            static_cast<std::uint64_t>(byte - '0');
        if (result >
            (std::numeric_limits<std::uint64_t>::max() - digit) /
                10U) {
            return Status::InvalidArgument(
                "file number overflows uint64");
        }
        result = result * 10U + digit;
    }
    if (result == 0U) {
        return Status::InvalidArgument(
            "CairnKV file number must be nonzero");
    }
    *file_number = result;
    return Status::Ok();
}

}  // namespace

Status MakeWalFileName(
    std::uint64_t file_number,
    bool temporary,
    std::string* name) {
    return MakeNumberedName(
        "WAL-", ".log", file_number, temporary, name);
}

Status MakeSstableFileName(
    std::uint64_t file_number,
    bool temporary,
    std::string* name) {
    return MakeNumberedName(
        "SST-", ".sst", file_number, temporary, name);
}

Status MakeManifestFileName(
    std::uint64_t file_number,
    bool temporary,
    std::string* name) {
    return MakeNumberedName(
        "MANIFEST-", "", file_number, temporary, name);
}

std::string CurrentFileName(bool temporary) {
    return temporary ? "CURRENT.tmp" : "CURRENT";
}

Status ParseFileName(
    std::string_view name,
    ParsedFileName* parsed) {
    if (parsed == nullptr) {
        return Status::InvalidArgument(
            "file-name parser received a null output");
    }
    const std::string_view original = name;
    const bool temporary = name.ends_with(".tmp");
    if (temporary) {
        name.remove_suffix(4U);
    }
    if (name == "CURRENT") {
        *parsed = ParsedFileName{FileKind::kCurrent, 0U, temporary};
        return Status::Ok();
    }

    struct Format {
        std::string_view prefix;
        std::string_view suffix;
        FileKind kind;
    };
    const Format formats[] = {
        {"WAL-", ".log", FileKind::kWal},
        {"SST-", ".sst", FileKind::kSstable},
        {"MANIFEST-", "", FileKind::kManifest},
    };
    for (const Format& format : formats) {
        if (name.size() != format.prefix.size() + 20U + format.suffix.size() ||
            !name.starts_with(format.prefix) || !name.ends_with(format.suffix)) {
            continue;
        }
        std::uint64_t file_number = 0U;
        const Status status = ParseFileNumber(
            name.substr(format.prefix.size(), 20U), &file_number);
        if (status.ok()) {
            *parsed = ParsedFileName{format.kind, file_number, temporary};
        }
        return status;
    }

    return Status::InvalidArgument(
        "invalid CairnKV file name: '" + std::string(original) + "'");
}

}  // namespace minilsm
