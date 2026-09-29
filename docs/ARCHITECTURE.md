# Architecture and source map

This document maps CairnKV's main storage-engine concepts to the implementation and summarizes the invariants that connect them.

## Source map

| Concept | Primary files | Key invariant |
|---|---|---|
| Public API / WriteBatch | `include/minilsm/db.hpp`, `include/minilsm/write_batch.hpp`, `include/minilsm/options.hpp` | A batch becomes visible atomically after its configured WAL durability step succeeds |
| DB write / rotation / flush / close | `src/db/db_impl.cpp` | Active and immutable WALs remain recoverable until their records are durably installed |
| Recovery / directory ownership | `src/db/db_lifecycle.cpp` | The database lock is acquired before recovery; `CURRENT` selects authority; validation precedes cleanup |
| Metadata installation | `src/db/file_install.cpp` | Durable output files precede MANIFEST publication; obsolete files are removed only after successful installation |
| WAL | `src/wal/` | A complete frame is validated before any mutation from it is replayed |
| Memtable / internal keys | `src/memtable/`, `src/common/internal_key.*` | User key ascending, sequence descending, record type deterministic |
| Point reads | `src/db/db_get.cpp` | Memory is checked before disk; L0 is searched newest first; lower levels have at most one range candidate |
| Range reads | `src/db/db_scan.cpp`, `src/iterator/` | Owned memory state and pinned table readers feed a lazy ordered merge |
| SSTables | `src/sstable/` | Immutable checksummed blocks, sparse index, Bloom filter, footer, and validated ordering |
| Caches | `src/cache/` | Cache eviction removes residency without invalidating a live shared owner |
| Compaction selection | `src/compaction/` | Level pressure drives selection; L0 uses complete transitive overlap closure |
| Compaction execution | `src/db/db_compaction.cpp` | Inputs are merged in internal-key order; outputs preserve user-key boundaries and safe tombstone rules |

## Write path

```text
Put / Delete / WriteBatch
        |
        v
validate batch
        |
        v
allocate contiguous sequence numbers
        |
        v
append one WAL frame
        |
        +---- sync=true ----> fsync WAL
        |
        v
publish all mutations to active memtable
        |
        v
memtable threshold reached?
        |
        v
rotate active -> immutable
        |
        v
background worker flushes immutable memtable to L0
        |
        v
background compaction moves data through L1-L3
```

`Put` and `Delete` use the same path as a single-mutation `WriteBatch`.

The engine supports one active memtable and at most one immutable memtable. If the active memtable reaches its threshold while the immutable slot is still occupied, writers wait on a condition variable. This is the engine's primary backpressure mechanism.

## Read path

```text
Get(key)
   |
   +--> active memtable
   |
   +--> immutable memtable
   |
   +--> overlapping L0 files, newest first
   |
   +--> one candidate in L1
   |
   +--> one candidate in L2
   |
   +--> one candidate in L3
```

For an SSTable candidate:

```text
TableCache
   -> Bloom filter
   -> sparse block index
   -> BlockCache
   -> pread + checksum + decode on cache miss
   -> internal-key seek
```

A visible tombstone terminates the search for that user key because older values must remain hidden.

## Range scans

A scan captures its in-memory sources and table handles before returning the iterator. The in-memory maps used by the scan are owned by the iterator, and SSTable readers are pinned with shared ownership.

The iterator layer merges sorted internal-key streams with a heap, then collapses versions by user key and suppresses tombstones.

```text
active-memory copy --------\
immutable-memory copy ------+--> MergingIterator --> user-visible iterator
L0/L1/L2/L3 iterators -----/
```

This gives stable iterator lifetime across background flush, compaction, `Close()`, and DB destruction. It is not a reusable MVCC snapshot facility.

## SSTable layout

SSTables contain immutable, checksummed structures:

```text
data blocks
index block
Bloom filter block
footer / metadata
```

Data blocks optionally use prefix compression. Each encoded record stores the shared prefix length, unshared suffix length, value length, sequence number, type, suffix bytes, and value bytes. Every 16th record is a restart point with a zero shared-prefix length.

A decoded block retains restart indexes. Seek first binary-searches restart keys, then scans the selected restart group.

## Caches and ownership

### TableCache

Maps an SSTable file number to an immutable `SstableReader`. A cached reader owns the file handle and table-level metadata such as the sparse index and Bloom filter.

### BlockCache

Maps an SSTable block identity to a decoded immutable block.

Both caches use bounded LRU residency. Reads and scans may retain `shared_ptr` references after an entry is evicted from the cache. Therefore cache capacity limits resident cache entries, not the total resources temporarily pinned by active operations.

The table-cache mutex may cover a cold table open. The DB state mutex covers source capture and the acquisition of the table handles needed by an operation. Data-block reads, checksum validation, and block decoding happen without holding the DB state mutex.

## Levels and compaction

L0 may contain overlapping files. L1-L3 maintain non-overlapping sorted user-key ranges.

Compaction pressure is based on:

```text
L0 score = L0 file count / L0 trigger
Ln score = level bytes / target bytes, for L1 and L2
```

L0 compaction starts from a selected L0 file and expands to the complete transitive closure of overlapping L0 ranges before adding overlapping L1 files. This prevents a remaining L0 file from shadowing data that was incorrectly moved below it.

Lower-level compactions use a remembered key pointer so selection advances through the key space rather than repeatedly starting at the first file.

Outputs never split a user-key group. Tombstones are retained whenever deeper levels may still contain an older value. The current implementation also conservatively retains the highest-sequence tombstone when required by manifest high-water validation.

## Metadata authority

`CURRENT` names the one authoritative MANIFEST snapshot. A newer MANIFEST filename is not authoritative by itself.

File installation follows the rule:

```text
new data file durable
    -> replacement MANIFEST durable
    -> CURRENT updated
    -> directory sync confirms authority
    -> obsolete files may be removed
```

If a failure makes the durability of an already-visible `CURRENT` replacement uncertain, the DB latches an error and stops accepting successful writes that would depend on that uncertain state.

## Concurrency model

CairnKV intentionally uses a limited model:

- one serialized writer path
- concurrent readers
- one background maintenance thread
- one DB state mutex and condition variable for lifecycle/maintenance coordination
- one mutex per concrete cache

The worker builds SSTables and compaction outputs outside the DB state mutex. Metadata installation is coordinated so only one maintenance job edits persistent version state at a time.

For the detailed synchronization rules, see [CONCURRENCY.md](CONCURRENCY.md).
