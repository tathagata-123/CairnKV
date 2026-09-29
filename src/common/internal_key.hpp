#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "minilsm/status.hpp"

namespace minilsm {

// Numeric values are part of the persisted version 1 format and must not be
// changed without a format-version decision.
enum class RecordType : std::uint8_t {
    kTombstone = 0,
    kValue = 1,
};

struct InternalKey {
    std::string user_key;
    std::uint64_t sequence_number = 0U;
    RecordType record_type = RecordType::kTombstone;
};

// Returns -1, 0, or 1 using the frozen unsigned-byte lexicographic order.
// Manifest range checks use the same helper as InternalKeyComparator.
[[nodiscard]] int CompareUserKeys(
    std::string_view left,
    std::string_view right);

class InternalKeyComparator {
public:
    // Returns -1, 0, or 1 using user key ascending (unsigned bytes),
    // sequence descending, then TOMBSTONE before VALUE.
    [[nodiscard]] int Compare(
        const InternalKey& left,
        const InternalKey& right) const;

    [[nodiscard]] bool operator()(
        const InternalKey& left,
        const InternalKey& right) const;
};

// Version 1 encoding:
//   u32 user_key_length (little-endian)
//   byte[user_key_length] user_key
//   u64 sequence_number (little-endian, 1 through UINT64_MAX)
//   u8 record_type (0 TOMBSTONE, 1 VALUE)
// The encoding is intentionally not order-preserving; callers must use
// InternalKeyComparator rather than comparing encoded bytes.
Status EncodeInternalKey(
    const InternalKey& key,
    std::string* encoded);
Status DecodeInternalKey(
    std::string_view encoded,
    InternalKey* key);

[[nodiscard]] std::string InternalKeyDebugString(
    const InternalKey& key);

}  // namespace minilsm
