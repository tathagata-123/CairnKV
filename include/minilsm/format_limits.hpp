#pragma once

#include <cstddef>
#include <cstdint>

namespace minilsm {

// Version 1 persisted-format bounds. These constants are shared by validation
// code so later file readers cannot silently choose different limits.
inline constexpr std::uint32_t kMaxUserKeyBytes = 1U * 1024U * 1024U;
inline constexpr std::uint32_t kMaxValueBytes = 64U * 1024U * 1024U;

inline constexpr std::size_t kMinSstableBlockTargetBytes = 1U * 1024U;
inline constexpr std::size_t kMaxSstableBlockTargetBytes = 64U * 1024U * 1024U;
inline constexpr std::size_t kDefaultSstableBlockTargetBytes = 4U * 1024U;

// The memtable limit counts encoded InternalKey bytes plus value bytes. It is
// an in-memory flush trigger rather than a persisted-format field.
inline constexpr std::size_t kDefaultMemtableBytesLimit = 4U * 1024U * 1024U;

// Version 1 maintenance thresholds are in-memory policy settings, not
// persisted format fields. The fixed policy has only these three triggers.
inline constexpr std::size_t kDefaultL0CompactionFileTrigger = 4U;
inline constexpr std::uint64_t kDefaultL1MaxBytes = 8U * 1024U * 1024U;
inline constexpr std::uint64_t kDefaultL2MaxBytes = 32U * 1024U * 1024U;
inline constexpr std::size_t kDefaultCompactionOutputFileTargetBytes =
    2U * 1024U * 1024U;

inline constexpr std::size_t kMinInternalKeyBytes = 13U;
inline constexpr std::size_t kMaxInternalKeyBytes =
    kMinInternalKeyBytes + kMaxUserKeyBytes;

inline constexpr std::uint64_t kMaxDatabaseFileBytes =
    1ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL;

inline constexpr std::size_t kMaxManifestFileBytes = 64U * 1024U * 1024U;
inline constexpr std::uint32_t kMaxManifestLiveFiles = 100'000U;

}  // namespace minilsm
