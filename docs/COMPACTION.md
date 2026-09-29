# Leveled compaction

L0 may overlap. L1, L2 and L3 have sorted, non-overlapping inclusive user-key ranges. L3 is bottommost. Trigger scores are L0 file count divided by its trigger, and lower-level live bytes divided by the configured target. The modern picker chooses the highest eligible score. L0 becomes eligible at its count trigger; lower levels exceed their byte target.

For L0, start with the oldest file and repeatedly include every L0 file overlapping the growing range until no new file joins. Include all overlapping L1 files. This fixed point is essential: leaving an older overlapping L0 while moving a newer version into L1 would break a level-ordered Get. Disjoint L0 runs can remain untouched.

For L1/L2, select the first file past the remembered compacted key, wrapping to the beginning when necessary. Include every destination overlap. Update the scheduling pointer only after successful installation. Pointers need not survive restart because they affect fairness, not correctness.

The executor streams a heap merge of input iterators, consumes every version of one user key, and retains its newest record. Output splitting happens after the complete key group. Tombstones are retained above bottommost because deeper older values may survive. Bottommost compaction can drop a tombstone after including its destination overlaps, except for the conservative highest-sequence record retained by the existing manifest high-water invariant.

Outputs are built/fsynced, renamed and directory-synced before a new complete manifest references them. CURRENT replacement and directory sync precede input deletion. Cache residency is invalidated for obsolete file numbers; already-pinned POSIX readers remain valid. Uninstalled outputs after failure are harmless orphans and cannot become authority from their filenames alone.

The single worker prevents overlapping compaction jobs. While a job builds outside the state mutex, foreground rotation waits through `maintenance_busy_`; writes can continue within the active memtable. This keeps full-Version installation safe without introducing an edit-log or concurrent manifest transaction layer.

Counters report exact selected input file bytes and installed output file bytes; they are logical engine work, not hardware I/O. See BENCHMARKS.md for amplification definitions.
