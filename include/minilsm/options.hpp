#pragma once

#include <cstddef>
#include <cstdint>

#include "minilsm/format_limits.hpp"
#include "minilsm/status.hpp"

namespace minilsm {

struct Options {
    bool create_if_missing = true;
    bool background_maintenance = true;
    // Compatibility fixture only; benchmarks leave this false.
    bool legacy_compaction_for_testing = false;
    bool paranoid_checks = false;
    bool prefix_compression = true;
    std::size_t block_cache_bytes = 8U * 1024U * 1024U;
    std::size_t table_cache_entries = 64U;

    // Physical data-block target used by SSTable builders. Entries are
    // never split, so one legal entry may produce an oversized block.
    std::size_t sstable_block_target_bytes =
        kDefaultSstableBlockTargetBytes;

    // The active memtable is rotated once this many logical record bytes are
    // present. A value larger than the limit is accepted and then flushed as
    // a one-record memtable.
    std::size_t memtable_bytes_limit = kDefaultMemtableBytesLimit;

    // Leveled compaction policy. L0 compacts when its file count
    // reaches the trigger. L1 and L2 compact while their total live bytes
    // exceed their respective limits. L3 is bottommost and has no trigger.
    std::size_t l0_compaction_file_trigger =
        kDefaultL0CompactionFileTrigger;
    std::uint64_t l1_max_bytes = kDefaultL1MaxBytes;
    std::uint64_t l2_max_bytes = kDefaultL2MaxBytes;

    // Approximate logical-entry target for each compaction output SSTable.
    // A user-key group is completed before a file boundary is considered.
    std::size_t compaction_output_file_target_bytes =
        kDefaultCompactionOutputFileTargetBytes;

    [[nodiscard]] Status Validate() const;
};

}  // namespace minilsm
