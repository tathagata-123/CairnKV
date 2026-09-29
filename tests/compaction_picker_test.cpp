#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "compaction/compaction.hpp"
#include "test_harness.hpp"

namespace {

minilsm::FileMetaData File(
    std::uint64_t number, std::uint32_t level,
    std::string smallest, std::string largest) {
    minilsm::FileMetaData file;
    file.file_number = number;
    file.level = level;
    file.file_size = 200U;
    file.entry_count = 1U;
    file.smallest_sequence = 1U;
    file.largest_sequence = 1U;
    file.smallest_user_key = std::move(smallest);
    file.largest_user_key = std::move(largest);
    return file;
}

minilsm::Version EmptyVersion() {
    minilsm::Version version;
    version.manifest_file_number = 90U;
    version.active_wal_file_number = 91U;
    version.next_file_number = 100U;
    return version;
}

bool HighestScoreAndThresholds() {
    auto version = EmptyVersion();
    version.last_sequence = 1U;
    version.levels[0] = {File(10U, 0U, "a", "b"), File(11U, 0U, "y", "z")};
    version.levels[1] = {
        File(20U, 1U, "a", "b"), File(21U, 1U, "c", "d"),
        File(22U, 1U, "e", "f")};
    version.levels[2] = {File(30U, 2U, "a", "b"), File(31U, 2U, "c", "d")};
    minilsm::Options options;
    options.l0_compaction_file_trigger = 2U;
    options.l1_max_bytes = 200U;
    options.l2_max_bytes = 100U;
    const std::array<std::string, minilsm::kLevelCount> pointers{};
    std::optional<minilsm::CompactionPlan> plan;

    MINILSM_CHECK(minilsm::PickCompaction(options, version, &plan).ok());
    MINILSM_CHECK(plan && plan->source_level == 0U);
    MINILSM_CHECK(plan->source_files.size() == 2U); // Legacy whole-L0 contract.
    MINILSM_CHECK(minilsm::PickCompaction(options, version, pointers, &plan).ok());
    MINILSM_CHECK(plan && plan->source_level == 2U); // Scores: 1, 3, 4.
    options.l0_compaction_file_trigger = 3U;
    MINILSM_CHECK(minilsm::PickCompaction(options, version, &plan).ok());
    MINILSM_CHECK(plan && plan->source_level == 1U); // Legacy fixed priority.
    options.l0_compaction_file_trigger = 2U;
    options.l2_max_bytes = 400U;
    MINILSM_CHECK(minilsm::PickCompaction(options, version, pointers, &plan).ok());
    MINILSM_CHECK(plan && plan->source_level == 1U);
    options.l1_max_bytes = 600U;
    MINILSM_CHECK(minilsm::PickCompaction(options, version, pointers, &plan).ok());
    MINILSM_CHECK(plan && plan->source_level == 0U);
    MINILSM_CHECK(plan->source_files.size() == 1U); // Unrelated L0 file stays.
    options.l0_compaction_file_trigger = 3U;
    MINILSM_CHECK(minilsm::PickCompaction(options, version, pointers, &plan).ok());
    MINILSM_CHECK(!plan); // Lower levels compact only strictly above target.
    return true;
}

bool TransitiveL0ClosureAndDestinationOverlaps() {
    auto version = EmptyVersion();
    version.last_sequence = 1U;
    version.levels[0] = {
        File(10U, 0U, "m", "n"), File(11U, 0U, "a", "b"),
        File(12U, 0U, "g", "h"), File(13U, 0U, "h", "m"),
        File(14U, 0U, "n", "p"), File(15U, 0U, "y", "z")};
    version.levels[1] = {
        File(20U, 1U, "a", "k"), File(21U, 1U, "l", "r"),
        File(22U, 1U, "t", "z")};
    minilsm::Options options;
    options.l0_compaction_file_trigger = 2U;
    const std::array<std::string, minilsm::kLevelCount> pointers{};
    std::optional<minilsm::CompactionPlan> plan;
    MINILSM_CHECK(minilsm::PickCompaction(options, version, pointers, &plan).ok());
    MINILSM_CHECK(plan && plan->source_level == 0U);
    MINILSM_CHECK(plan->source_files.size() == 4U);
    const std::array<std::uint64_t, 4> expected{10U, 12U, 13U, 14U};
    for (std::size_t i = 0U; i < expected.size(); ++i) {
        MINILSM_CHECK(plan->source_files[i].file_number == expected[i]);
    }
    MINILSM_CHECK(plan->source_smallest_user_key == "g");
    MINILSM_CHECK(plan->source_largest_user_key == "p");
    MINILSM_CHECK(plan->smallest_user_key == "a");
    MINILSM_CHECK(plan->largest_user_key == "r");
    MINILSM_CHECK(plan->overlapping_destination_files.size() == 2U);
    MINILSM_CHECK(plan->overlapping_destination_files[0].file_number == 20U);
    MINILSM_CHECK(plan->overlapping_destination_files[1].file_number == 21U);
    // Inclusive endpoints and the second closure pass are both necessary.
    // Neither remaining L0 file can contain a version of a selected source key.
    for (const auto index : {1U, 5U}) {
        const auto& file = version.levels[0][index];
        MINILSM_CHECK(!minilsm::UserKeyRangesOverlap(
            file.smallest_user_key, file.largest_user_key,
            plan->source_smallest_user_key, plan->source_largest_user_key));
    }
    return true;
}

bool PointerAdvancesWrapsAndIsNotMutated() {
    auto version = EmptyVersion();
    version.last_sequence = 1U;
    version.levels[1] = {
        File(20U, 1U, "", ""), File(21U, 1U, "b", "d"),
        File(22U, 1U, "f", "h")};
    minilsm::Options options;
    options.l1_max_bytes = 200U;
    std::array<std::string, minilsm::kLevelCount> pointers{};
    std::optional<minilsm::CompactionPlan> plan;
    MINILSM_CHECK(minilsm::PickCompaction(options, version, pointers, &plan).ok());
    MINILSM_CHECK(plan && plan->source_files.front().file_number == 20U);
    pointers[1] = "c"; // A newly inserted file spanning the pointer is eligible.
    MINILSM_CHECK(minilsm::PickCompaction(options, version, pointers, &plan).ok());
    MINILSM_CHECK(plan && plan->source_files.front().file_number == 21U);
    MINILSM_CHECK(pointers[1] == "c");
    pointers[1] = "d";
    MINILSM_CHECK(minilsm::PickCompaction(options, version, pointers, &plan).ok());
    MINILSM_CHECK(plan && plan->source_files.front().file_number == 22U);
    pointers[1] = "h";
    MINILSM_CHECK(minilsm::PickCompaction(options, version, pointers, &plan).ok());
    MINILSM_CHECK(plan && plan->source_files.front().file_number == 20U);

    version.levels[1].clear();
    const std::string byte7f(1U, static_cast<char>(0x7f));
    const std::string byte80(1U, static_cast<char>(0x80));
    version.levels[2] = {
        File(30U, 2U, byte7f, byte7f), File(31U, 2U, byte80, byte80)};
    options.l2_max_bytes = 200U;
    pointers[2] = byte7f;
    MINILSM_CHECK(minilsm::PickCompaction(options, version, pointers, &plan).ok());
    MINILSM_CHECK(plan && plan->source_files.front().file_number == 31U);
    return true;
}

bool InvalidInputAndEmptyVersion() {
    auto version = EmptyVersion();
    minilsm::Options options;
    std::array<std::string, minilsm::kLevelCount> pointers{};
    std::optional<minilsm::CompactionPlan> plan;
    MINILSM_CHECK(minilsm::PickCompaction(options, version, pointers, &plan).ok());
    MINILSM_CHECK(!plan);
    MINILSM_CHECK(!minilsm::PickCompaction(options, version, pointers, nullptr).ok());
    plan.emplace();
    options.l0_compaction_file_trigger = 0U;
    MINILSM_CHECK(!minilsm::PickCompaction(options, version, pointers, &plan).ok());
    MINILSM_CHECK(!plan);
    options.l0_compaction_file_trigger = 1U;
    version.next_file_number = 0U;
    MINILSM_CHECK(!minilsm::PickCompaction(options, version, pointers, &plan).ok());
    return true;
}

} // namespace

int main() {
    MINILSM_RUN(HighestScoreAndThresholds);
    MINILSM_RUN(TransitiveL0ClosureAndDestinationOverlaps);
    MINILSM_RUN(PointerAdvancesWrapsAndIsNotMutated);
    MINILSM_RUN(InvalidInputAndEmptyVersion);
    return 0;
}
