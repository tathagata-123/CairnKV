# Deliberate limitations

- POSIX/local-filesystem durability model; process-crash tests do not certify power-loss behavior.
- No reusable MVCC snapshots. Scans own a creation-time view but cannot share a snapshot across independent operations.
- No transactions beyond batch atomicity, secondary indexes, SQL, networking or replication.
- Single maintenance worker; full active memory waits through a long compaction. Metadata work and cold table opens may hold the DB state mutex.
- Four fixed levels, full manifest snapshots, one immutable slot; no production throttling or dynamic sizing.
- Resident cache bounds exclude extra objects pinned by live readers/scans and approximate allocator overhead. Large scans pin multiple table descriptors.
- Memtable threshold is a trigger; a bounded 128MiB batch can exceed it. Legal large records still need substantial memory. Allocation failure is not recovered through Status.
- Cold prefix blocks are fully decoded; restarts accelerate searches within decoded blocks, not partial on-disk decoding.
- Legacy WAL length corruption can be ambiguous with a torn final frame; new batch framing protects its length separately.
- Interrupted initial database creation can leave a nonempty directory with no CURRENT and requires explicit cleanup. Unknown files are never silently adopted or deleted.
- Highest-sequence tombstones may be retained conservatively for existing metadata compatibility.
- No Windows abstraction, direct I/O, hardware I/O accounting, repair utility, or production qualification.

These boundaries concentrate study on storage algorithms and correctness rather than feature count.
