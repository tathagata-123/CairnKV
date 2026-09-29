# Concurrency and lifetimes

CairnKV serializes writers with `writer_mutex_`. `mutex_` protects active memory, immutable ownership, current Version, sequence allocation, error/stop state and counters. The condition variable waits on predicates protected by that state mutex. Each concrete cache has its own mutex. There is exactly one maintenance thread.

## Writes and waits

A writer holds writer serialization across condition-variable waits. It validates inputs, appends the complete WAL frame, applies all mutations while holding the state mutex, and advances sequence allocation. No Get/Scan can capture a partly applied batch. A batch can overshoot the memtable threshold; it is a flush trigger, not a strict memory ceiling. The WAL batch envelope is bounded at 128 MiB and one million mutations.

When the active table fills, rotation requires no immutable table and no maintenance job in progress. Otherwise the writer waits; background errors wake it. Rotation syncs the old WAL and publishes the two-WAL manifest before switching active memory. The worker builds the immutable table and ordinary compactions outside the state mutex. Writers may fill the new active table during that work.

The `maintenance_busy_` rule also blocks foreground WAL rotation during unlocked compaction work. Therefore copied Version metadata and file-number allocation cannot be invalidated by another rotation. This is simpler than allowing concurrent metadata edits and merging their deltas. It can lengthen a full-memtable stall during a long compaction cascade.

## Reads

Get captures its memory result or pins all disk range candidates under the state mutex. It then releases the mutex for block I/O. Table readers are immutable after cache configuration and use independent positional reads. Read counters are accumulated under the state mutex.

Scan copies active/immutable record maps at creation and pins table readers under the same lock. It merges lazily after releasing the lock. The copies are bounded by the pending memory sources, except for oversized batches. A Scan remains valid across future writes, flushes, compactions, Close and database object destruction. Its own methods require single-threaded use.

A live iterator can retain file descriptors and blocks beyond LRU residency. Unlinked POSIX files remain readable through the held descriptors. Never infer a hard global descriptor cap from `table_cache_entries`. When another database is opened later, it has distinct caches; file-number cache keys are scoped to one DB instance.

## Shutdown and errors

Close serializes against writers, drains accepted state, signals worker termination and joins without holding the worker's state mutex. WAL and ownership lock are released after work stops. Callers must check Close's Status; destructor cleanup cannot report it. Concurrent destruction of a DB object while other threads call its methods is outside the API contract.

A background failure is latched and wakes waiters. An uncertain directory-sync result after CURRENT replacement also stops future writes and cleanup based on that uncertain authority. Recovery synchronizes the selected visible directory state before obsolete-file cleanup. The engine does not guarantee recovery from C++ allocation failure; resource exhaustion is a process-level failure model.

Synchronous maintenance is available for deterministic tests and controlled mechanism comparisons. Some deterministic tests use a compatibility compaction configuration to preserve exact topology assertions; normal foreground and background operation use the modern compaction picker.

In deterministic synchronous-maintenance mode, a failed `Close()` retains the open object so the operation can be retried. Default background mode reports the error after stopping the worker and releasing the directory lock. Destroying the object always releases RAII resources, but callers should check the explicit `Close()` result when successful shutdown matters.
