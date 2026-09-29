# Current scope

CairnKV is an educational C++20 LSM-tree key-value storage engine. Its scope is intentionally limited to the storage mechanisms needed to study durability, sorted immutable storage, caching, compaction, recovery, and basic background maintenance.

## Implemented

### Write path

- `Put`
- `Delete`
- atomic `WriteBatch`
- monotonic sequence numbers
- synchronous or asynchronous WAL durability policy
- one active memtable
- one immutable memtable
- hard writer backpressure
- one background maintenance worker

### Persistence and recovery

- checksummed WAL frames
- complete-frame batch replay
- active and immutable WAL recovery
- complete MANIFEST snapshots
- `CURRENT` authority switching
- file and directory synchronization ordering
- orphan cleanup after authority validation
- POSIX database-directory lock
- normal and paranoid startup validation

### SSTables and reads

- block-based SSTables
- checksummed data blocks
- sparse block index
- Bloom filters
- prefix-compressed user keys
- restart points
- point lookup
- forward range scan
- internal sequence/tombstone visibility rules
- bounded table cache
- bounded decoded-block cache

### Compaction

- L0 plus three non-overlapping lower levels
- leveled compaction
- level-pressure scoring
- transitive L0 overlap closure
- lower-level compaction pointer
- streaming merge
- user-key-safe output boundaries
- tombstone retention/drop rules for the supported no-snapshot model

### Validation and evaluation

- unit and format tests
- corruption tests
- crash-transition tests
- randomized differential tests
- cache and prefix-block tests
- targeted concurrency tests
- sanitizer configurations
- deterministic parser mutation testing
- benchmark drivers for batching, caching, compression, and maintenance
- optional LevelDB comparison workload

## Deliberate simplifications

The project deliberately favors clear algorithms over production-level generality:

- `std::map` is used for memtables rather than a concurrent skip list;
- writes are serialized;
- there is one background maintenance thread rather than a worker pool;
- only one immutable memtable may be pending;
- backpressure is simple waiting rather than a multi-stage rate controller;
- there are four fixed levels rather than dynamic level sizing;
- only one leveled compaction policy is implemented;
- MANIFEST files are complete snapshots rather than an incremental version-edit log;
- cache replacement uses straightforward LRU structures;
- cold compressed blocks are fully decoded rather than searched through a fully lazy compressed cursor.

## Not implemented

CairnKV does not currently provide:

- reusable MVCC snapshots
- general transactions or transactional reads
- secondary indexes
- SQL or relational execution
- reverse iteration
- range tombstones
- column families
- TTL
- blob/value-log storage
- multiple compaction strategies
- dynamic level sizing
- custom allocators
- direct I/O or `io_uring`
- lock-free data structures
- replication
- Raft
- sharding
- networking/RPC
- production repair or administration tools
- Windows portability for the current POSIX locking/file assumptions

## Resource-model limits

Cache capacities bound entries resident in the caches, not every object held by active operations. A long-running scan can pin SSTable readers and decoded blocks after they have been evicted from cache residency.

A legal large batch can also temporarily exceed the configured memtable threshold before rotation. The engine therefore provides bounded mechanisms suitable for the intended educational workloads, not a strict global memory governor.

For additional implementation boundaries, see [LIMITATIONS.md](LIMITATIONS.md).
