# Atomic WriteBatch WAL extension

`WriteBatch` owns an ordered vector of puts/deletes. `DB::Write` assigns consecutive
sequence numbers in vector order. A batch with repeated keys therefore has a
well-defined newest mutation. Empty batches do not consume sequence numbers.

The WAL keeps its 32-byte `MLSMWAL1` file header and can contain both the original
single-mutation frames and the batch extension below. An older-format binary
rejects the extension as an invalid legacy payload length; it cannot accidentally
replay a prefix as independent operations. Files containing this extension require
the revised engine.

All integers are unsigned little-endian. Checksums use CRC32C.

| Field | Size | Meaning |
|---|---:|---|
| Marker | 4 bytes | `0xffffffff`, outside the valid legacy payload-size range |
| Payload length | 4 bytes | Bytes from starting sequence through final mutation |
| Header checksum | 4 bytes | CRC32C of the preceding marker and length |
| Starting sequence | 8 bytes | Nonzero first sequence |
| Mutation count | 4 bytes | Number of ordered mutations |
| Mutations | Variable | Repeated records in the following table |
| Frame checksum | 4 bytes | CRC32C of every preceding byte in this frame |

| Mutation field | Size | Meaning |
|---|---:|---|
| Type | 1 byte | `0` delete; `1` put |
| Key length | 4 bytes | At most 1 MiB |
| Value length | 4 bytes | At most 64 MiB; zero for delete |
| Key | Key length | Arbitrary key bytes |
| Value | Value length | Arbitrary value bytes |

A frame contains 1–1,000,000 mutations, with payload between 21 bytes and 128 MiB.
A mutation receives `starting_sequence + mutation_index`. Encoding and decoding
reject sequence overflow, invalid types/lengths, and trailing bytes. The encoder
checks the complete batch before building its frame. The decoder bounds the count
by both the configured maximum and the bytes physically present before reserving
its output vector. These limits bound parser allocations; ordinary workload batches
should be much smaller.

The reader verifies the fixed header checksum before trusting the payload length.
A complete header declaring an out-of-range size is corruption, even if the file
is short. An incomplete final header or a valid header followed by an incomplete
payload/checksum is a torn tail. A complete frame with a bad checksum or malformed
mutation is corruption. The older single-record framing retains its original
unchecked-length/torn-tail ambiguity.

The complete batch is decoded and validated into a temporary vector before
`WalReader::ReadNext` exposes its first mutation. It then returns the validated
mutations individually to preserve the existing recovery interface. `valid_size()`
advances over the complete frame only after validation; a torn frame contributes
zero records. Recovery truncates the permitted torn tail before appending.

`AppendBatch(first_sequence, batch, sync)` appends one logical frame. With
`sync=true`, it fsyncs once for the whole batch before reporting success. With
`sync=false`, logical atomicity is unchanged, but a machine crash may lose the
batch. `WalWriter::Sync()` makes preceding appended frames durable; DB rotation
and successful close use explicit synchronization. An append/fsync failure poisons
that writer. A failed write can have an uncertain durable outcome, so callers must
not interpret its error as proof that recovery will omit the batch.

The WAL guarantees complete-frame recovery. The DB mutex additionally protects
publication of the entire batch to the memtable so readers cannot observe only a
prefix during normal execution. Memory-allocation exhaustion is not a recoverable
transaction mechanism.

`wal_batch_test` covers mixed legacy/batch replay, every byte truncation boundary
of an example frame, header and payload corruption, valid-checksum malformed
mutations/counts, sequence overflow, oversized declared envelopes, and sync-failure
poisoning. These are parser/process tests, not a hardware power-cut simulation.
