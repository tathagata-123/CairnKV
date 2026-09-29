# WAL, authority and recovery

## WriteBatch

The public batch owns its strings. Each mutation gets one sequence number in insertion order; duplicate keys within a batch resolve by the largest sequence. Empty batches are no-ops. A batch is encoded into one bounded WAL frame with a protected length header and whole-frame CRC. See WAL_BATCH_FORMAT.md for exact byte layout and compatibility.

The synchronous write order is frame append → fsync → whole-batch memtable publication. Unsynchronized writes retain atomic visibility but may be lost after machine failure. Syncing a later batch or sealing the WAL synchronizes preceding bytes too. Append/fsync failure poisons the WAL writer. An error does not prove the attempted bytes were absent; retry policy must account for an indeterminate durable outcome.

Recovery verifies an entire frame and every contained record before returning any member. An incomplete active-WAL final frame contributes no mutations. Complete corrupt frames fail recovery. Legacy singleton WAL frames remain readable, with their documented length/torn-tail ambiguity. New batch headers protect the declared length before the reader uses it.

## Rotation and flush

Before switching active memory, sync the old WAL, create/fsync a fresh WAL, sync the directory, publish a full manifest naming old immutable WAL and new active WAL, then replace/sync CURRENT. Only then accept mutations that depend on the new WAL identity.

The worker builds and synchronizes an SSTable from immutable memory. Its next manifest adds the table, removes the immutable-WAL reference and retains the current active WAL. After CURRENT is durably installed, immutable memory can be released and obsolete WAL/manifest files deleted. A crash before publication leaves the old WALs authoritative; a crash afterward uses the new table and remaining active WAL.

Compaction uses the same durable-output-before-authority rule. No filename heuristic chooses the newest-looking manifest or SSTable.

## Reopen

Acquire the database LOCK before reading, replaying or deleting state. Follow CURRENT, validate its manifest and all referenced table metadata (all data too in paranoid mode), then replay immutable WAL followed by active WAL with strictly increasing sequence ranges. A sealed immutable WAL has already been synchronized and must not have a torn tail. Only the active tail may be truncated to its last complete verified frame.

Raise the file allocator above surviving numbered artifacts before deleting recognized orphans; never reuse an attempted ID while a file may remain. Preserve unknown files and the persistent LOCK pathname. Synchronize selected visible directory authority before cleanup. A nonempty directory without CURRENT fails safely: interrupted initial creation may require removing the uncommitted directory manually.

## Error model

CURRENT rename is a visible authority change even if the following directory sync fails. In-memory state must match that visible selection, but new writes and destructive cleanup must stop until recovery resolves it. CairnKV latches this state and tests the post-rename failure separately from pre-rename failure.

Named child-process crash tests validate process recovery and file authority transitions. They do not simulate actual power loss, controller caches, every filesystem behavior, or arbitrary torn device writes. Correctness relies on ordinary POSIX fsync/rename/locking semantics on a local filesystem. There is no repair tool and no claim of production qualification.
