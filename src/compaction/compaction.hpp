#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "manifest/version.hpp"
#include "minilsm/options.hpp"
#include "minilsm/status.hpp"

namespace minilsm {

// An immutable description chosen from one authoritative Version. Source and
// destination metadata are copied before execution mutates any live state.
struct CompactionPlan {
    std::uint32_t source_level = 0U;
    std::uint32_t destination_level = 0U;
    std::vector<FileMetaData> source_files;
    std::vector<FileMetaData> overlapping_destination_files;

    // Destination overlap is selected against this inclusive source range.
    std::string source_smallest_user_key;
    std::string source_largest_user_key;

    // Inclusive union of every selected source and destination input.
    std::string smallest_user_key;
    std::string largest_user_key;
};

// Returns the single next plan in fixed priority order: L0, then L1, then L2.
// A successful empty optional means that every trigger is currently satisfied.
Status PickCompaction(
    const Options& options,
    const Version& version,
    std::optional<CompactionPlan>* plan);

// Current policy: choose the highest eligible level score. L0 selects the
// oldest file and its transitive L0 overlaps; L1+ advances through keyspace.
// Empty pointers mean "start at the first file". Update a pointer to the
// plan's source_largest_user_key only after successful metadata installation.
// Pointers are scheduling hints; they do not need to survive a restart.
Status PickCompaction(
    const Options& options,
    const Version& version,
    const std::array<std::string, kLevelCount>& last_compacted_key,
    std::optional<CompactionPlan>* plan);

[[nodiscard]] bool UserKeyRangesOverlap(
    std::string_view left_smallest,
    std::string_view left_largest,
    std::string_view right_smallest,
    std::string_view right_largest);

}  // namespace minilsm
