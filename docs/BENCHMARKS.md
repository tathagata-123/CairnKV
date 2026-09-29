# Mechanism experiments

The measurements below were collected on 29 September 2026 using GCC 13.3 with `-O3` on a Linux 6.18.44 x86_64 development environment. Each table is from one fixed-seed trial (`20260929`) with fresh CairnKV application caches but an uncontrolled/warm operating-system page cache.

These numbers are intended to illustrate the behavior of individual mechanisms. They are **not** hardware-normalized production benchmarks. Re-run the benchmark program on the target machine before making performance claims.

Build a Release configuration and run:

```sh
build/release/cairnkv_experiments --quick
build/release/cairnkv_experiments
```

The benchmark source is `tools/cairnkv_experiments.cpp`.

## Write batching

Workload:

- 20,000 logical mutations
- 33-byte common-prefix keys
- 100-byte values
- `sync=true`
- preconstructed batches
- large memtable to minimize flush/compaction interference

Throughput counts logical mutations. Latency percentiles measure a whole public `Write()` call, not an individual mutation inside a batch. The expected sync-request count is derived from the number of batch calls; it is not a kernel-level `fsync` trace.

| Batch size | Mutations/s | Expected batch sync requests | p50 call (µs) | p95 call (µs) | p99 call (µs) |
|---:|---:|---:|---:|---:|---:|
| 1 | 227,298 | 20,000 | 3.42 | 5.07 | 12.63 |
| 10 | 374,809 | 2,000 | 24.24 | 31.79 | 92.83 |
| 100 | 362,396 | 200 | 237.04 | 497.90 | 946.54 |
| 1,000 | 381,779 | 20 | 2454.51 | 3984.58 | 4101.99 |

Grouping mutations reduces the number of WAL synchronization points while preserving complete-frame recovery and atomic batch visibility. Larger batches perform more work per public call, so call latency rises even when mutation throughput improves.

The filesystem used for this run made synchronization unusually inexpensive. The absolute rates should not be extrapolated to a physical SSD without rerunning the workload there.

## Block cache

Workload:

- 30,000 distinct keys
- 100,000 uniform random successful Gets
- prefix compression enabled
- fresh CairnKV block cache for each capacity
- OS page cache not dropped between cases

| Capacity | Gets/s | Hit rate | Data-block read requests | Evictions | p50 (µs) | p95 (µs) | p99 (µs) |
|---|---:|---:|---:|---:|---:|---:|---:|
| Disabled | 17,901 | 0.00% | 100,000 | 0 | 42.65 | 77.72 | 252.55 |
| 16 KiB | 20,146 | 0.22% | 99,784 | 99,782 | 42.71 | 67.56 | 163.76 |
| 256 KiB | 21,031 | 4.14% | 95,857 | 95,819 | 42.46 | 65.84 | 162.34 |
| 4 MiB | 51,451 | 64.81% | 35,188 | 34,575 | 2.01 | 45.05 | 104.09 |

The 4 MiB cache eliminated 64,812 of 100,000 CairnKV data-block read requests in this trace. The small caches provide little reuse under uniform random access, so their modest throughput changes should not be treated as robust performance gains.

A CairnKV "data-block read request" means an actual engine `pread` request for a data block. The request can still be served from the operating-system page cache; it does not imply a physical device read.

The table cache recorded 34 misses and 99,966 hits in each case. Candidate-table read amplification was 1.00 table per successful Get for this prepared non-overlapping dataset. That number is not representative of arbitrary L0 overlap or unsuccessful lookup workloads.

## Prefix compression

The same 30,000-key / 100,000-Get workload was used with the CairnKV block cache disabled in both configurations. The scan covers all keys. Compressed and uncompressed layouts can place different numbers of records in a target-sized data block.

| Prefix blocks | SSTable bytes | Bytes/key | Gets/s | Scan keys/s | Get p99 (µs) |
|---|---:|---:|---:|---:|---:|
| Off | 4,757,410 | 158.58 | 21,077 | 400,706 | 150.19 |
| On | 3,856,100 | 128.54 | 20,355 | 502,325 | 164.24 |

On this shared-prefix workload, prefix compression reduced stored SSTable bytes by **18.95%**.

The point-read result does not show a latency improvement: a cold compressed block is still reconstructed before lookup, and the observed p99 was slightly worse with compression in this single trial. Scan throughput was higher in the recorded run. The strongest conclusion supported by this experiment is the deterministic reduction in stored bytes; latency/throughput conclusions require repeated measurements.

## Foreground versus background maintenance

Workload:

- 30,000 synchronous mutations
- small memtable and level targets to create compaction pressure
- overwrites included
- same compaction picker, cache settings, compression settings, and input sequence in both modes
- `Close()` drain time included separately so unfinished background work is not hidden

| Maintenance | Foreground mutations/s | Including drain/s | p50 call (µs) | p95 (µs) | p99 (µs) | Close drain (ms) |
|---|---:|---:|---:|---:|---:|---:|
| Foreground | 13,248 | 13,242 | 3.03 | 5.22 | 125.06 | 1.00 |
| Background | 9,226 | 9,219 | 3.26 | 11.01 | 455.37 | 2.70 |

In this run, background maintenance was slower and had worse p99 write latency. Therefore the current background worker should **not** be described as a measured tail-latency optimization.

Its purpose in CairnKV is architectural: it separates foreground writes from flush/compaction work and demonstrates immutable-memtable lifecycle, synchronization, and bounded backpressure.

Possible contributors to the measured slowdown include:

- extra durable WAL-rotation metadata;
- worker scheduling and lock contention;
- one immutable-memtable slot;
- writers waiting while compaction cascades occupy the maintenance worker.

These are implementation hypotheses, not experimentally isolated causes.

Both modes selected 39,203,723 bytes of compaction input and wrote 36,215,653 bytes of compaction output, ending with 1,811,201 live SSTable bytes. The recorded compaction-output amplification was 9.0766x under the definition below.

## Metric definitions

| Metric | Definition |
|---|---|
| Mutation throughput | Logical user mutations divided by timed seconds; all mutations in a batch are counted |
| API latency | Wall-clock duration of one public write/Get call; nearest-rank p50/p95/p99 |
| Read amplification shown | SSTable candidates actually examined divided by Get requests |
| Block-cache hit rate | Hits divided by hits + misses for decoded data-block cache lookups |
| Compaction input bytes | File sizes of the selected compaction inputs |
| Compaction output bytes | File sizes of successfully installed compaction outputs |
| Compaction-only write amplification | Compaction output bytes divided by logical key+value bytes written |
| Live SSTable space amplification | Live SSTable bytes divided by logical bytes of live unique key/value records |
| Database size in comparison tool | Total regular-file bytes in the prepared database directory |

`Compaction-only write amplification` intentionally excludes WAL, initial flush, MANIFEST, filesystem metadata, and device-level writes. It is **not** total engine or hardware write amplification.

Likewise, the benchmark does not perform syscall tracing or device-I/O tracing. Metrics are named for the work CairnKV itself can count directly.

## Optional LevelDB comparison

`tools/compare_leveldb.cpp` implements one workload against either the CairnKV or LevelDB public API.

Using the direct build driver:

```sh
python3 tools/build_and_test.py --mode release --build-dir build/release-python
python3 tools/run_leveldb_comparison.py --build-dir build/release-python --quick
```

With CMake, enable:

```sh
-DCAIRNKV_COMPARE_LEVELDB=ON
```

when LevelDB development headers and libraries are available.

The matched configuration uses:

- synchronous writes
- 100-byte values
- fixed query sequence
- 4 MiB cache
- 4 KiB target blocks
- 128 KiB write buffer
- Bloom filter at 10 bits/key
- LevelDB Snappy compression disabled

LevelDB still uses its own native algorithms and prefix-compression implementation. LevelDB read preparation uses `CompactRange()` because its public API does not expose a direct flush equivalent; CairnKV uses `Close()`/flush. Preparation time is therefore reported separately.

No LevelDB numbers are included in this document because the dependency was not available in the environment used for the measurements above. A one-engine run is not presented as a comparison. Run both branches in the same environment before drawing relative-performance conclusions.
