# Prefix compression and caches

CairnKV keeps the SSTable sparse index, full-table Bloom filter, and footer layouts. Data blocks independently identify their encoding. Readers accept original `MDB1` blocks and new `MDB2` blocks. Low-level builders default to legacy encoding to preserve exact-format tests; DB flush and compaction builders pass the configured prefix-compression option.

## Data-block format

All integers are little-endian fixed-width fields. A physical block contains:

| Field | Bytes |
|---|---:|
| Body length | 4 |
| Body | body length |
| CRC32C of length and body | 4 |

The prefix body begins with `MDB2`, `u16 version=2`, `u16 reserved=0`, and `u32 entry_count`. Each entry is:

| Field | Bytes |
|---|---:|
| Shared user-key prefix length | 4 |
| Unshared user-key suffix length | 4 |
| Value length | 4 |
| Sequence number | 8 |
| Record type | 1 |
| User-key suffix | suffix length |
| Value | value length |

The user key is reconstructed from the previous user-key prefix and the stored suffix. Sequence numbers and record types remain explicit; comparison still uses user key ascending, sequence descending, then record type. Encoded bytes are never compared as InternalKeys.

Entries 0, 16, 32, and so on are restart entries with shared length zero. The entries are followed by one `u32` byte offset per restart and a final `u32 restart_count`. Offsets are relative to the beginning of the entry region, immediately after the 12-byte body header. The checksum covers this trailer too.

For example, keys `customer:000001` and `customer:000002` can share `customer:00000`. A restart stores the complete user key so reconstruction can begin without the preceding group.

The parser checks exact lengths, checksum, magic/version, key/value limits, sequence/type validity, tombstones, strict InternalKey order, and duplicate user-key/sequence records. It also checks that restart offsets are increasing, point to actual entry boundaries, match the fixed interval and count, and select entries with no shared bytes. A shared prefix cannot exceed the previous key length. Trailing bytes are rejected.

Reconstructed entries retain the legacy aggregate 66 MiB data-body limit, in addition to physical-block bounds. This prevents a small sequence of shared-prefix records from expanding into an unbounded aggregate of copied keys. The SSTable builder flushes before crossing this limit.

## Seeking and its deliberate trade-off

A cold read checks and decodes the whole block into ordinary `DataBlockEntry` values once. The decoded block retains the restart entry indices. `LowerBound` binary-searches restart keys to select the final restart at or before the target, then scans at most one 16-entry group. Exact keys, gaps, versions, and targets outside the block use the same InternalKey comparator.

This implementation makes restart navigation explicit while keeping the existing, easily inspected decoded representation. It does not implement lazy on-demand prefix reconstruction inside a cold block. Compression saves stored key bytes; the decoded block cache amortizes reconstruction on repeated reads. Warm blocks retain complete keys and therefore consume more memory than their compressed bytes on disk.

## Two different caches

| Cache | Key | Cached value | Capacity |
|---|---|---|---|
| Table cache | SSTable file number | Reader, descriptor, sparse index, Bloom metadata | Reader count |
| Block cache | File number and block offset | Validated immutable decoded block | Estimated decoded bytes |

Both caches use `std::list`, `std::unordered_map`, and one mutex. Lookup moves an entry to the front. Insertion evicts entries at the back until the resident capacity is respected. An oversize decoded block is read normally but is not retained. A zero capacity disables retention while preserving read counters.

A DB owns its caches; do not share one block cache between unrelated database directories with overlapping file numbers. SSTable file numbers are not reused within an open DB instance. Recovery raises the allocator past surviving numbered artifacts; after reopen the caches are fresh. An abandoned number can be forgotten across a later crash after its orphan was deleted, but no cache survives that reopen. A new table therefore cannot obtain an old instance's block even if both use block offset zero.

Shared ownership keeps an in-flight reader or block valid after eviction. Cache capacity bounds resident entries, not memory or descriptors pinned by callers. The block charge includes the decoded object, vector capacity, and string capacities; it is a useful bound on accounted decoded storage, not exact allocator or cache-container overhead.

On a cache miss, I/O and decoding happen outside the block-cache mutex. Concurrent misses can read and decode the same block twice. Insertion checks for an existing entry under the mutex, so it neither duplicates resident keys nor double-charges them. Avoiding this small redundant work would require extra coordination that is unnecessary here.

`Erase(file_number)` removes resident blocks for an obsolete table; held shared blocks remain valid. An already-running old reader may insert a block after erasure. This remains a harmless bounded cache entry because file numbers are not reused within that open instance. File deletion safety comes from the DB's state-mutex capture and shared-reader lifetime rules, not from the cache mutex.

The table cache closes an evicted descriptor when the last shared reader releases it. POSIX permits an already-open descriptor to read a file after unlink. The DB holds its state mutex while opening and capturing all relevant shared table readers, preventing deletion before they are open. It releases that mutex for data I/O. There is no separate Version-pin abstraction.

## Internal API and statistics

- `SstableBuilder::Create(path, block_target, &builder, prefix_compression)` selects the data encoding.
- `SstableReader::SetBlockCache(cache, file_number)` attaches one DB-local cache before the reader is shared. It is not a concurrent reconfiguration method.
- `ReadSharedDataBlock(index, &block)` returns `shared_ptr<const DataBlockReader>`. SSTable iterators use this path.
- `ReadDataBlock(index, &block)` preserves the legacy copy-out helper for validators and low-level tests.
- `BlockCache::Lookup`, `Insert`, `Erase`, and `Clear` maintain the resident LRU.
- `BlockCache::RecordRead(bytes)` counts a successfully completed data-block read before decoding; a checksum-failing read still represents I/O.
- `BlockCache::Stats()` returns a synchronized snapshot: `hits`, `misses`, `evictions`, `physical_reads`, `bytes_read`, and `resident_bytes`.
- `TableCache::Get(file_number, &reader)` returns a shared reader; `stats()` exposes hits, misses, evictions, and resident entries.

Here “physical read” means a data-block read requested through the file reader. The operating system may satisfy it from the page cache; the metric does not count actual device operations. It excludes footer/index/filter metadata reads. Table readers not attached to a block-cache statistics object do not contribute to these counters. `Clear` and `Erase` change residency without resetting cumulative statistics or counting explicit removal as capacity eviction.

## Tests

`prefix_block_test` compares restart seeks against an independent lower-bound reference across keys, versions, boundaries, and gaps; verifies compression on shared prefixes; covers empty/no-prefix keys; and rejects malformed/truncated restart blocks. Existing block/SSTable/corruption tests retain their legacy-format coverage.

`block_cache_test` covers hits, misses, LRU eviction, oversize/disabled capacity behavior, file identity, pinned lifetimes, shared read reuse, and actual data-read counter changes. `table_cache_test` checks reader reuse, eviction, final-owner release, unlink safety, and disabled caching.
