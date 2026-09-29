# Storage formats

All numeric fields use the explicit little-endian codec used throughout CairnKV. This document describes the current persisted formats, validation rules, and compatibility behavior.

| Structure | Current behavior |
|---|---|
| CURRENT | Existing fixed 30-byte checksummed authority record; names one manifest |
| MANIFEST | Existing complete snapshot v1 when one WAL is sufficient; v2 extends its header by 8 bytes for immutable WAL number; record layout and trailing CRC retained |
| WAL header | Existing 32-byte `MLSMWAL1` header |
| WAL frame | Legacy single-mutation frames accepted; new sentinel batch frames protect both header length and full frame |
| Data block | Legacy `MDB1` accepted; prefix/restart `MDB2` supported with strict decoding |
| Index/filter/footer | Existing sparse index, whole-table Bloom and footer framing preserved |

A two-WAL manifest keeps magic `MLSMMAN1`, sets version=2, header size=64 rather than56, and appends immutable-WAL u64 after the original header fields and before live-file records. The immutable and active WAL IDs must be distinct, valid allocated numbers. The complete snapshot checksum covers the extension. Do not claim old binaries can open new files: they reject unsupported structures.

See [WAL_BATCH_FORMAT.md](WAL_BATCH_FORMAT.md) for the complete batch frame and [PREFIX_AND_CACHES.md](PREFIX_AND_CACHES.md) for prefix/restart and cache details. Every new decoder retains explicit lengths, count bounds, checksums, sequence/type/order validation and malformed-input failure status.

Normal startup trusts checksummed manifest key/sequence metadata that is not duplicated in table metadata; it does not prove every data record agrees without reading the blocks. Paranoid mode performs that full consistency check. Checksums detect accidental corruption, not adversarial forgery.
