# Testing and reproducibility

CairnKV has 33 test executables covering the storage formats, WAL and batch semantics, SSTables, compaction, caches, recovery, corruption handling, asynchronous maintenance, randomized histories, and parser robustness.

## Tested configurations

The current test suite has been exercised in the following configurations:

| Configuration | Compiler flags / observed result |
|---|---|
| Debug | GCC 13.3, C++20, `-O0 -g -Wall -Wextra -Wpedantic -Werror -pthread`; 33/33 executables passed |
| Release | GCC 13.3, C++20, strict warnings, `-O3`; 33/33 executables passed |
| ASan + UBSan | `-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined`; 33/33 executables passed |
| ThreadSanitizer | Targeted `db_revision_test` suite passed without reported races |

The ASan/UBSan run used `ASAN_OPTIONS=detect_leaks=0` because LeakSanitizer was not usable in that test environment. Therefore this result is **not** a leak-check pass.

Passing tests and sanitizers provide evidence for the exercised schedules and inputs; they are not a proof of correctness over every possible crash, race, allocation failure, or storage-device behavior.

## Running the suite

### CMake presets

Debug:

```sh
cmake --preset debug
cmake --build --preset debug -j2
ctest --preset debug --output-on-failure
```

Release:

```sh
cmake --preset release
cmake --build --preset release -j2
ctest --preset release --output-on-failure
```

ASan + UBSan:

```sh
cmake --preset asan-ubsan
cmake --build --preset asan-ubsan -j2
ctest --preset asan-ubsan --output-on-failure
```

### Direct compiler driver

A Python build/test driver is included for environments where the CMake/Ninja workflow is inconvenient:

```sh
python3 tools/build_and_test.py --mode debug --build-dir build/debug-python
python3 tools/build_and_test.py --mode release --build-dir build/release-python --bench
ASAN_OPTIONS=detect_leaks=0 python3 tools/build_and_test.py --sanitize --build-dir build/asan-python
python3 tools/build_and_test.py --tsan --build-dir build/tsan-python --filter db_revision
```

The GitHub Actions workflow runs the primary build/test configurations in CI.

## Coverage by subsystem

### WAL and WriteBatch

Tests cover:

- legacy and batch WAL frames;
- multiple puts/deletes in one batch;
- contiguous sequence assignment;
- duplicate keys inside a batch;
- checksum/header corruption;
- malformed types/counts;
- sequence overflow checks;
- every truncation offset of a batch frame;
- mixed frame replay;
- atomic replay of a complete batch;
- rejection of invalid complete records;
- active-WAL torn-tail behavior;
- sealed immutable-WAL corruption behavior.

### SSTables

Tests cover:

- data block encoding/decoding;
- block checksums;
- sparse indexes;
- Bloom filters;
- footer/metadata validation;
- sorted InternalKey ordering;
- randomized SSTable contents;
- corrupted block/index/filter/footer inputs;
- seek and iteration boundaries.

### Prefix-compressed blocks

Tests cover:

- keys with shared prefixes;
- keys without useful shared prefixes;
- restart boundaries;
- seeks before the first key;
- seeks between keys;
- seeks after the last key;
- malformed restart arrays;
- invalid offsets/lengths;
- ordering validation.

### Table and block caches

Tests cover:

- cache hit/miss behavior;
- LRU eviction;
- capacity accounting;
- pinned readers/blocks that outlive cache residency;
- duplicate concurrent fills;
- cache reuse across reads;
- invalidation of obsolete resident entries after metadata installation.

### Compaction

Tests cover:

- level-pressure selection;
- L0 transitive overlap closure;
- lower-level selection and key-pointer progression;
- non-overlapping L1-L3 output ranges;
- user-key-safe output splitting;
- version collapsing;
- tombstone retention and safe dropping;
- reopen after compaction;
- deterministic configurations that exercise exact topology-sensitive cases.

### DB lifecycle and concurrency

The DB-focused test suite covers:

- a second process failing to acquire the database lock;
- scan lifetime through flush, compaction, `Close()`, and DB destruction;
- concurrent Gets/scans observing batch-consistent state;
- invalid batches publishing no visible prefix;
- cache use through normal reads;
- latched error handling after an uncertain `CURRENT` sync;
- background-worker failure wakeup;
- startup with pending compaction pressure;
- clean shutdown and worker join behavior.

### Crash transitions

Crash tests terminate the process at selected durability boundaries including:

- durable memtable/WAL rotation;
- temporary SSTable completion;
- final SSTable rename;
- immutable-table installation.

Reopen checks that acknowledged data and complete synchronized batches survive according to the documented authority rules. Reopening more than once also checks that recovery does not depend on one-time incidental state.

These tests use process termination and the host filesystem. They test CairnKV's metadata authority and framing rules but do not emulate every real power-loss or controller-cache failure mode.

### Randomized differential testing

Seeded randomized histories execute combinations of:

- `Put`;
- `Delete`;
- `Get`;
- `Scan`;
- memtable rotation;
- background flush;
- compaction;
- close/reopen.

Observed key/value state is compared against a reference model. The asynchronous test configurations use small thresholds so that flush and compaction behavior is exercised frequently.

### Parser mutation testing

A deterministic parser test mutates structured seeds and truncates valid inputs across:

- WAL frames;
- `CURRENT`;
- MANIFEST data;
- SSTable data blocks;
- index blocks;
- Bloom-filter data;
- SSTable footer metadata.

The suite executes 20,000 mutation cases in addition to valid and truncation seeds. An optional libFuzzer entry point is included for longer coverage-guided fuzzing, but no claim is made that a long exhaustive fuzzing campaign has been completed.

## Test limitations

The suite does not comprehensively cover:

- real disk-full behavior;
- every allocation-failure point;
- operating-system/filesystem matrices;
- storage-controller cache loss;
- arbitrary reordered durable writes outside the documented POSIX/fsync model;
- production-duration endurance testing;
- all possible thread schedules.

These are outside the intended scope of the educational engine. The tests focus on the explicit invariants CairnKV documents and implements.
