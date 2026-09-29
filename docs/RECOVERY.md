# Recovery

CairnKV recovery is driven by explicit metadata authority rather than by scanning the directory and guessing which files are current.

For the complete WAL and crash-ordering specification, see [WAL_AND_RECOVERY.md](WAL_AND_RECOVERY.md).

## Authority model

`CURRENT` names the authoritative MANIFEST. The MANIFEST is a complete snapshot of the persistent database state required for recovery, including:

- live SSTable metadata
- sequence metadata
- the active WAL identity
- the immutable WAL identity when a memtable flush is pending

A newer MANIFEST or SSTable filename is not considered committed merely because it exists.

## Open sequence

At a high level, `DB::Open()` performs:

```text
acquire database LOCK
    -> read CURRENT
    -> read and validate selected MANIFEST
    -> open/validate referenced table metadata
    -> replay the WALs named by the MANIFEST
    -> restore active/immutable in-memory state
    -> synchronize recovery state as required
    -> remove recognized unreferenced files
    -> start background maintenance
```

The directory lock is acquired before recovery and cleanup so that two processes cannot independently modify the same database directory.

## WAL replay

A WAL frame is validated before its mutations are applied. Validation includes the frame format, checksum, record types, sequence ordering, and batch structure.

A complete `WriteBatch` frame replays atomically: recovery never exposes only a prefix of a valid batch.

The active WAL may end with an incomplete final frame, which is treated as a torn tail. A sealed immutable WAL is expected to be complete; an incomplete tail in that WAL is treated as corruption rather than silently discarded.

## Memtable rotation and two WALs

When the active memtable is rotated, the old memtable becomes immutable and remains associated with its WAL. A fresh active WAL is created for subsequent writes.

Before new active-WAL mutations are allowed to depend on the new state, persistent metadata is updated so recovery knows about both WALs:

```text
old WAL -> immutable memtable
new WAL -> active memtable
```

The immutable WAL remains named until its records have been written to an SSTable and a replacement MANIFEST has been durably published.

## Flush installation

A background flush follows this ordering:

```text
build temporary SSTable
    -> fsync SSTable
    -> rename to final filename
    -> fsync directory
    -> write replacement MANIFEST
    -> fsync MANIFEST
    -> replace CURRENT
    -> fsync directory
    -> release immutable state
    -> remove files that are no longer referenced
```

A crash before authority switches leaves the old MANIFEST authoritative. A crash after a confirmed authority switch causes recovery to follow the new MANIFEST.

## Compaction installation

Compaction follows the same authority principle:

1. build and synchronize replacement SSTables;
2. create a complete replacement MANIFEST describing the new file set;
3. publish it through `CURRENT`;
4. only after successful installation, remove obsolete input files.

Readers that already hold an open SSTable handle remain safe across POSIX unlink because their file descriptor continues to reference the existing file object.

## Uncertain metadata-sync failures

A particularly important failure case is:

```text
CURRENT rename succeeds
but the following directory fsync fails
```

The pathname may already expose the new authority while its crash durability is uncertain. CairnKV treats this as a latched error state. It does not continue acknowledging writes or perform cleanup that assumes either side is definitely durable. Reopening the database is required to re-establish authority from the filesystem state.

## Startup validation modes

### Normal mode

Normal startup validates:

- `CURRENT`
- the selected MANIFEST
- table footer/index/filter metadata
- referenced WAL structure during replay

Individual data-block checksums are verified when those blocks are read.

### Paranoid mode

With `paranoid_checks=true`, startup additionally reads and validates every referenced data block and cross-checks table contents against persisted metadata and Bloom-filter expectations.

## Recovery limits

The implementation intentionally does not claim to model every storage-device failure. In particular:

- process-termination tests do not emulate all forms of real power loss;
- device write-cache reordering is outside the test model;
- disk-full and exhaustive allocation-failure schedules are not comprehensively exercised;
- an interrupted initial database creation can leave a nonempty directory without a valid `CURRENT`, which requires cleanup rather than filename-based reconstruction.

These constraints keep recovery rules explicit without introducing production repair tooling.
