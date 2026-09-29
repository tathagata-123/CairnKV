#include "minilsm/options.hpp"

#include <string>

namespace minilsm {

Status Options::Validate() const {
    if (sstable_block_target_bytes < kMinSstableBlockTargetBytes ||
        sstable_block_target_bytes > kMaxSstableBlockTargetBytes) {
        return Status::InvalidArgument(
            "sstable_block_target_bytes must be between " +
            std::to_string(kMinSstableBlockTargetBytes) + " and " +
            std::to_string(kMaxSstableBlockTargetBytes));
    }
    if (memtable_bytes_limit == 0U ||
        memtable_bytes_limit > kMaxDatabaseFileBytes) {
        return Status::InvalidArgument(
            "memtable_bytes_limit must be between 1 and " +
            std::to_string(kMaxDatabaseFileBytes));
    }
    if (l0_compaction_file_trigger == 0U ||
        l0_compaction_file_trigger > kMaxManifestLiveFiles) {
        return Status::InvalidArgument(
            "l0_compaction_file_trigger must be between 1 and " +
            std::to_string(kMaxManifestLiveFiles));
    }
    if (l1_max_bytes == 0U || l1_max_bytes > kMaxDatabaseFileBytes) {
        return Status::InvalidArgument(
            "l1_max_bytes must be between 1 and " +
            std::to_string(kMaxDatabaseFileBytes));
    }
    if (l2_max_bytes == 0U || l2_max_bytes > kMaxDatabaseFileBytes) {
        return Status::InvalidArgument(
            "l2_max_bytes must be between 1 and " +
            std::to_string(kMaxDatabaseFileBytes));
    }
    if (compaction_output_file_target_bytes == 0U ||
        compaction_output_file_target_bytes > kMaxDatabaseFileBytes) {
        return Status::InvalidArgument(
            "compaction_output_file_target_bytes must be between 1 and " +
            std::to_string(kMaxDatabaseFileBytes));
    }
    return Status::Ok();
}

}  // namespace minilsm
