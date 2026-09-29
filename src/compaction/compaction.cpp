#include "compaction/compaction.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/internal_key.hpp"
#include "manifest/version.hpp"

namespace minilsm {

namespace {

Status LevelBytes(
    const std::vector<FileMetaData>& files,
    std::uint64_t* bytes) {
    std::uint64_t total = 0U;
    for (const FileMetaData& file : files) {
        if (file.file_size >
            std::numeric_limits<std::uint64_t>::max() - total) {
            return Status::Corruption("level byte total overflows uint64");
        }
        total += file.file_size;
    }
    *bytes = total;
    return Status::Ok();
}

void ExtendRange(
    const FileMetaData& file,
    std::string* smallest,
    std::string* largest) {
    if (CompareUserKeys(file.smallest_user_key, *smallest) < 0) {
        *smallest = file.smallest_user_key;
    }
    if (CompareUserKeys(file.largest_user_key, *largest) > 0) {
        *largest = file.largest_user_key;
    }
}

void SelectDestinationOverlaps(
    const Version& version,
    CompactionPlan* plan) {
    for (const FileMetaData& file :
         version.levels[plan->destination_level]) {
        if (!UserKeyRangesOverlap(
                plan->source_smallest_user_key,
                plan->source_largest_user_key,
                file.smallest_user_key,
                file.largest_user_key)) {
            continue;
        }
        plan->overlapping_destination_files.push_back(file);
        ExtendRange(
            file, &plan->smallest_user_key, &plan->largest_user_key);
    }
}

CompactionPlan MakePlan(
    const Version& version,
    std::uint32_t source_level,
    std::vector<FileMetaData> source_files) {
    CompactionPlan plan;
    plan.source_level = source_level;
    plan.destination_level = source_level + 1U;
    plan.source_files = std::move(source_files);
    plan.source_smallest_user_key =
        plan.source_files.front().smallest_user_key;
    plan.source_largest_user_key =
        plan.source_files.front().largest_user_key;
    for (const FileMetaData& file : plan.source_files) {
        ExtendRange(
            file,
            &plan.source_smallest_user_key,
            &plan.source_largest_user_key);
    }
    plan.smallest_user_key = plan.source_smallest_user_key;
    plan.largest_user_key = plan.source_largest_user_key;
    SelectDestinationOverlaps(version, &plan);
    return plan;
}

std::vector<FileMetaData> SelectL0Closure(const Version& version) {
    const auto& files = version.levels[0];
    std::string smallest = files.front().smallest_user_key;
    std::string largest = files.front().largest_user_key;
    std::vector<bool> selected(files.size(), false);
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t i = 0U; i < files.size(); ++i) {
            if (!selected[i] && UserKeyRangesOverlap(
                    smallest, largest,
                    files[i].smallest_user_key, files[i].largest_user_key)) {
                selected[i] = true;
                ExtendRange(files[i], &smallest, &largest);
                changed = true;
            }
        }
    }
    // Keep ascending file-number order. Every unselected L0 range is disjoint
    // from this closure, so older L0 versions cannot shadow moved newer keys.
    std::vector<FileMetaData> inputs;
    for (std::size_t i = 0U; i < files.size(); ++i) {
        if (selected[i]) {
            inputs.push_back(files[i]);
        }
    }
    return inputs;
}

}  // namespace

bool UserKeyRangesOverlap(
    std::string_view left_smallest,
    std::string_view left_largest,
    std::string_view right_smallest,
    std::string_view right_largest) {
    return CompareUserKeys(left_largest, right_smallest) >= 0 &&
           CompareUserKeys(right_largest, left_smallest) >= 0;
}

namespace {

// A null pointer preserves the original synchronous picker's fixed-priority
// and whole-L0 behavior for its existing three-argument API.
Status PickCompactionImpl(
    const Options& options,
    const Version& version,
    const std::array<std::string, kLevelCount>* last_compacted_key,
    std::optional<CompactionPlan>* plan) {
    if (plan == nullptr) {
        return Status::InvalidArgument("compaction picker received a null output");
    }
    plan->reset();
    Status status = ValidateVersionMetadata(version);
    if (!status.ok()) {
        return status;
    }
    status = options.Validate();
    if (!status.ok()) {
        return status;
    }

    std::size_t chosen = kLevelCount;
    long double highest_score = 0.0L;
    if (version.levels[0].size() >= options.l0_compaction_file_trigger) {
        chosen = 0U;
        highest_score = static_cast<long double>(version.levels[0].size()) /
                        options.l0_compaction_file_trigger;
    }
    for (std::size_t level = 1U; level + 1U < kLevelCount; ++level) {
        if (last_compacted_key == nullptr && chosen != kLevelCount) {
            break;
        }
        std::uint64_t bytes = 0U;
        status = LevelBytes(version.levels[level], &bytes);
        if (!status.ok()) {
            return status;
        }
        const std::uint64_t target =
            level == 1U ? options.l1_max_bytes : options.l2_max_bytes;
        const long double score = static_cast<long double>(bytes) / target;
        if (bytes > target && score > highest_score) {
            chosen = level;
            highest_score = score;
        }
    }
    if (chosen == kLevelCount) {
        return Status::Ok();
    }
    if (chosen == 0U) {
        *plan = MakePlan(version, 0U, last_compacted_key == nullptr
            ? version.levels[0] : SelectL0Closure(version));
        return Status::Ok();
    }

    const auto& files = version.levels[chosen];
    std::size_t index = 0U;
    if (last_compacted_key != nullptr && !(*last_compacted_key)[chosen].empty()) {
        const std::string& pointer = (*last_compacted_key)[chosen];
        while (index < files.size() &&
               CompareUserKeys(files[index].largest_user_key, pointer) <= 0) {
            ++index;
        }
        if (index == files.size()) {
            index = 0U;
        }
    }
    *plan = MakePlan(
        version, static_cast<std::uint32_t>(chosen), {files[index]});
    return Status::Ok();
}

}  // namespace

Status PickCompaction(
    const Options& options,
    const Version& version,
    std::optional<CompactionPlan>* plan) {
    return PickCompactionImpl(options, version, nullptr, plan);
}

Status PickCompaction(
    const Options& options,
    const Version& version,
    const std::array<std::string, kLevelCount>& last_compacted_key,
    std::optional<CompactionPlan>* plan) {
    return PickCompactionImpl(options, version, &last_compacted_key, plan);
}

}  // namespace minilsm
