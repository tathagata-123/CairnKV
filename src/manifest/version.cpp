#include "manifest/version.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "minilsm/format_limits.hpp"
#include "sstable/sstable_format.hpp"

namespace minilsm {

namespace {

Status ValidateFileMetadata(
    const FileMetaData& file,
    std::size_t expected_level,
    std::uint64_t last_sequence) {
    if (file.level != expected_level || file.level >= kLevelCount) {
        return Status::Corruption(
            "MANIFEST SSTable has invalid level placement");
    }
    if (file.file_number == 0U) {
        return Status::Corruption(
            "MANIFEST SSTable file number is zero");
    }
    if (file.file_size < kMinSstableFileBytes ||
        file.file_size > kMaxDatabaseFileBytes) {
        return Status::Corruption(
            "MANIFEST SSTable size is outside version 1 bounds");
    }
    if (file.entry_count == 0U || file.smallest_sequence == 0U ||
        file.largest_sequence < file.smallest_sequence ||
        file.largest_sequence > last_sequence) {
        return Status::Corruption(
            "MANIFEST SSTable count or sequence range is invalid");
    }
    if (file.smallest_user_key.size() > kMaxUserKeyBytes ||
        file.largest_user_key.size() > kMaxUserKeyBytes ||
        CompareUserKeys(
            file.smallest_user_key, file.largest_user_key) > 0) {
        return Status::Corruption(
            "MANIFEST SSTable user-key range is invalid");
    }
    return Status::Ok();
}

}  // namespace

void SortLevelFilesByUserKey(std::vector<FileMetaData>* files) {
    if (files == nullptr) {
        return;
    }
    std::sort(
        files->begin(),
        files->end(),
        [](const FileMetaData& left, const FileMetaData& right) {
            const int smallest_order = CompareUserKeys(
                left.smallest_user_key, right.smallest_user_key);
            if (smallest_order != 0) {
                return smallest_order < 0;
            }
            const int largest_order = CompareUserKeys(
                left.largest_user_key, right.largest_user_key);
            if (largest_order != 0) {
                return largest_order < 0;
            }
            return left.file_number < right.file_number;
        });
}

Status ValidateLevelNonOverlap(
    const std::vector<FileMetaData>& files,
    std::uint32_t expected_level) {
    if (expected_level == 0U || expected_level >= kLevelCount) {
        return Status::InvalidArgument(
            "non-overlap validation requires level L1, L2, or L3");
    }
    const FileMetaData* previous = nullptr;
    for (const FileMetaData& file : files) {
        if (file.level != expected_level) {
            return Status::Corruption(
                "lower-level SSTable has invalid level placement");
        }
        if (previous != nullptr &&
            CompareUserKeys(
                previous->largest_user_key,
                file.smallest_user_key) >= 0) {
            return Status::Corruption(
                "MANIFEST lower-level SSTable ranges overlap");
        }
        previous = &file;
    }
    return Status::Ok();
}

Status ValidateVersionMetadata(const Version& version) {
    if (version.manifest_file_number == 0U ||
        version.active_wal_file_number == 0U ||
        version.next_file_number == 0U) {
        return Status::Corruption(
            "MANIFEST contains a zero required file number");
    }

    std::set<std::uint64_t> used_file_numbers;
    used_file_numbers.insert(version.manifest_file_number);
    if (!used_file_numbers.insert(version.active_wal_file_number).second) {
        return Status::Corruption(
            "MANIFEST reuses one file number for multiple live files");
    }

    std::uint64_t maximum_file_number =
        version.manifest_file_number > version.active_wal_file_number
            ? version.manifest_file_number
            : version.active_wal_file_number;
    if (version.immutable_wal_file_number != 0U) {
        if (!used_file_numbers.insert(version.immutable_wal_file_number).second) {
            return Status::Corruption("MANIFEST repeats immutable WAL number");
        }
        maximum_file_number = std::max(maximum_file_number, version.immutable_wal_file_number);
    }
    std::size_t file_count = 0U;
    std::uint64_t maximum_sequence = 0U;
    for (std::size_t level = 0U; level < kLevelCount; ++level) {
        const std::vector<FileMetaData>& files = version.levels[level];
        if (level > 0U) {
            const Status level_status = ValidateLevelNonOverlap(
                files, static_cast<std::uint32_t>(level));
            if (!level_status.ok()) {
                return level_status;
            }
        }
        const FileMetaData* previous = nullptr;
        for (const FileMetaData& file : files) {
            Status status =
                ValidateFileMetadata(file, level, version.last_sequence);
            if (!status.ok()) {
                return status;
            }
            if (!used_file_numbers.insert(file.file_number).second) {
                return Status::Corruption(
                    "MANIFEST contains a duplicate file number");
            }
            if (previous != nullptr) {
                if (level == 0U &&
                    previous->file_number >= file.file_number) {
                    return Status::Corruption(
                        "MANIFEST L0 files are not ordered by file number");
                }
            }
            if (file.file_number > maximum_file_number) {
                maximum_file_number = file.file_number;
            }
            if (file.largest_sequence > maximum_sequence) {
                maximum_sequence = file.largest_sequence;
            }
            previous = &file;
            ++file_count;
        }
    }

    if (file_count > kMaxManifestLiveFiles) {
        return Status::Corruption(
            "MANIFEST contains too many live SSTables");
    }
    if (file_count == 0U && version.last_sequence != 0U) {
        return Status::Corruption(
            "MANIFEST without SSTables has a nonzero last sequence");
    }
    if (file_count > 0U && maximum_sequence != version.last_sequence) {
        return Status::Corruption(
            "MANIFEST last sequence is not the highest installed sequence");
    }
    if (version.next_file_number <= maximum_file_number) {
        return Status::Corruption(
            "MANIFEST next file number is not above every live number");
    }
    return Status::Ok();
}

Status AllocateFileNumber(
    Version* version,
    std::uint64_t* file_number) {
    if (version == nullptr || file_number == nullptr) {
        return Status::InvalidArgument(
            "file-number allocation received a null argument");
    }
    if (version->next_file_number == 0U ||
        version->next_file_number ==
            std::numeric_limits<std::uint64_t>::max()) {
        return Status::IOError("file-number space is exhausted");
    }

    *file_number = version->next_file_number;
    ++version->next_file_number;
    return Status::Ok();
}

std::optional<std::size_t> FindFileByUserKeyRange(
    const std::vector<FileMetaData>& files,
    std::string_view user_key) {
    std::size_t first = 0U;
    std::size_t count = files.size();
    while (count > 0U) {
        const std::size_t step = count / 2U;
        const std::size_t middle = first + step;
        if (CompareUserKeys(
                files[middle].largest_user_key, user_key) < 0) {
            first = middle + 1U;
            count -= step + 1U;
        } else {
            count = step;
        }
    }

    if (first == files.size() ||
        CompareUserKeys(user_key, files[first].smallest_user_key) < 0) {
        return std::nullopt;
    }
    return first;
}

bool SstableOverlapsUserRange(
    const FileMetaData& file,
    const std::optional<std::string>& lower_bound,
    const std::optional<std::string>& upper_bound) {
    if (lower_bound.has_value() && upper_bound.has_value() &&
        CompareUserKeys(*lower_bound, *upper_bound) >= 0) {
        return false;
    }
    if (lower_bound.has_value() &&
        CompareUserKeys(file.largest_user_key, *lower_bound) < 0) {
        return false;
    }
    if (upper_bound.has_value() &&
        CompareUserKeys(file.smallest_user_key, *upper_bound) >= 0) {
        return false;
    }
    return true;
}

}  // namespace minilsm
