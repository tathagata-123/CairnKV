#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/internal_key.hpp"
#include "minilsm/status.hpp"

namespace minilsm {

inline constexpr std::size_t kLevelCount = 4U;

// The first nine fields are persisted in each MANIFEST live-file record.
// Boundary InternalKeys are derived by validating the referenced SSTable and
// are retained in memory for reads and compaction; version 1 does not
// duplicate their bytes in the MANIFEST.
struct FileMetaData {
    std::uint64_t file_number = 0U;
    std::uint32_t level = 0U;
    std::uint64_t file_size = 0U;
    std::uint64_t entry_count = 0U;
    std::uint64_t smallest_sequence = 0U;
    std::uint64_t largest_sequence = 0U;
    std::string smallest_user_key;
    std::string largest_user_key;
    InternalKey smallest_internal_key;
    InternalKey largest_internal_key;
};

// A Version is the complete authoritative metadata snapshot. L0 files are in
// ascending file-number order. L1-L3 files are in ascending non-overlapping
// user-key range order.
struct Version {
    std::uint64_t manifest_file_number = 0U;
    std::uint64_t active_wal_file_number = 0U;
    std::uint64_t immutable_wal_file_number = 0U;
    std::uint64_t last_sequence = 0U;
    std::uint64_t next_file_number = 0U;
    std::array<std::vector<FileMetaData>, kLevelCount> levels;
};

Status ValidateVersionMetadata(const Version& version);
Status AllocateFileNumber(Version* version, std::uint64_t* file_number);

// Direct invariant helpers. L1-L3 are kept in this deterministic order,
// and equality at adjacent endpoints is overlap rather than a gap.
void SortLevelFilesByUserKey(std::vector<FileMetaData>* files);
Status ValidateLevelNonOverlap(
    const std::vector<FileMetaData>& files,
    std::uint32_t expected_level);

// Files in L1-L3 are sorted by disjoint inclusive user-key ranges. This
// binary search returns the sole possible file or no candidate for a gap.
[[nodiscard]] std::optional<std::size_t> FindFileByUserKeyRange(
    const std::vector<FileMetaData>& files,
    std::string_view user_key);

// An SSTable range is inclusive, while a scan range is half-open. This
// helper is shared by every level so Scan never applies point-read or Bloom
// filter rules to range-source selection.
[[nodiscard]] bool SstableOverlapsUserRange(
    const FileMetaData& file,
    const std::optional<std::string>& lower_bound,
    const std::optional<std::string>& upper_bound);

}  // namespace minilsm
