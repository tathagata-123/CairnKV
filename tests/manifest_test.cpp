#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "common/checksum.hpp"
#include "common/coding.hpp"
#include "manifest/manifest.hpp"
#include "manifest/version.hpp"
#include "minilsm/status.hpp"
#include "sstable/sstable_format.hpp"
#include "test_harness.hpp"

namespace {

minilsm::FileMetaData File(
    std::uint64_t number,
    std::uint32_t level,
    std::string smallest,
    std::string largest,
    std::uint64_t smallest_sequence,
    std::uint64_t largest_sequence,
    std::uint64_t file_size = minilsm::kMinSstableFileBytes) {
    minilsm::FileMetaData file;
    file.file_number = number;
    file.level = level;
    file.file_size = file_size;
    file.entry_count = 3U;
    file.smallest_sequence = smallest_sequence;
    file.largest_sequence = largest_sequence;
    file.smallest_user_key = std::move(smallest);
    file.largest_user_key = std::move(largest);
    return file;
}

minilsm::Version ValidVersion() {
    minilsm::Version version;
    version.manifest_file_number = 10U;
    version.active_wal_file_number = 11U;
    version.last_sequence = 100U;
    version.next_file_number = 30U;
    version.levels[0].push_back(File(12U, 0U, "a", "c", 1U, 20U));
    version.levels[0].push_back(File(13U, 0U, "b", "d", 21U, 40U));
    version.levels[1].push_back(File(14U, 1U, "e", "g", 41U, 60U));
    version.levels[1].push_back(File(15U, 1U, "h", "z", 61U, 100U));
    return version;
}

void StoreFixed32At(
    std::string* bytes,
    std::size_t offset,
    std::uint32_t value) {
    std::string encoded;
    minilsm::PutFixed32(&encoded, value);
    bytes->replace(offset, encoded.size(), encoded);
}

void StoreFixed64At(
    std::string* bytes,
    std::size_t offset,
    std::uint64_t value) {
    std::string encoded;
    minilsm::PutFixed64(&encoded, value);
    bytes->replace(offset, encoded.size(), encoded);
}

void RefreshManifestChecksum(std::string* bytes) {
    StoreFixed32At(
        bytes,
        bytes->size() - 4U,
        minilsm::Crc32c(
            std::string_view(*bytes).substr(0U, bytes->size() - 4U)));
}

bool TestManifestRoundTripAndMinimumSstableSize() {
    const minilsm::Version expected = ValidVersion();
    std::string encoded;
    MINILSM_CHECK(
        minilsm::EncodeManifestSnapshot(expected, &encoded).ok());
    MINILSM_CHECK(encoded.substr(0U, 8U) == "MLSMMAN1");

    minilsm::Version decoded;
    MINILSM_CHECK(
        minilsm::DecodeManifestSnapshot(encoded, 10U, &decoded).ok());
    MINILSM_CHECK(decoded.manifest_file_number == 10U);
    MINILSM_CHECK(decoded.active_wal_file_number == 11U);
    MINILSM_CHECK(decoded.last_sequence == 100U);
    MINILSM_CHECK(decoded.next_file_number == 30U);
    MINILSM_CHECK(decoded.levels[0].size() == 2U);
    MINILSM_CHECK(decoded.levels[1].size() == 2U);
    MINILSM_CHECK(decoded.levels[2].empty());
    MINILSM_CHECK(decoded.levels[3].empty());
    MINILSM_CHECK(
        decoded.levels[0][0].file_size ==
        minilsm::kMinSstableFileBytes);
    MINILSM_CHECK(decoded.levels[1][1].largest_user_key == "z");
    return true;
}

bool TestCurrentRoundTripAndStrictGrammar() {
    std::string current;
    MINILSM_CHECK(minilsm::EncodeCurrent(42U, &current).ok());
    MINILSM_CHECK(
        current == "MANIFEST-00000000000000000042\n");
    std::uint64_t number = 0U;
    MINILSM_CHECK(minilsm::DecodeCurrent(current, &number).ok());
    MINILSM_CHECK(number == 42U);

    const std::string malformed[] = {
        "",
        "MANIFEST-00000000000000000042",
        "MANIFEST-00000000000000000042\r\n",
        "MANIFEST-0000000000000000004x\n",
        "WAL-00000000000000000000042\n",
        "MANIFEST-00000000000000000000\n",
    };
    for (const std::string& bytes : malformed) {
        MINILSM_CHECK(
            minilsm::DecodeCurrent(bytes, &number).code() ==
            minilsm::Status::Code::kCorruption);
    }
    return true;
}

bool ExpectManifestCorruption(
    std::string bytes,
    std::uint64_t expected_number = 10U) {
    minilsm::Version decoded;
    const minilsm::Status status = minilsm::DecodeManifestSnapshot(
        bytes, expected_number, &decoded);
    MINILSM_CHECK(status.code() == minilsm::Status::Code::kCorruption);
    return true;
}

bool TestManifestHeaderChecksumAndTruncation() {
    std::string valid;
    MINILSM_CHECK(
        minilsm::EncodeManifestSnapshot(ValidVersion(), &valid).ok());

    std::string bad_magic = valid;
    bad_magic[0] = 'X';
    RefreshManifestChecksum(&bad_magic);
    MINILSM_CHECK(ExpectManifestCorruption(bad_magic));

    std::string bad_version = valid;
    StoreFixed32At(&bad_version, 8U, 2U);
    RefreshManifestChecksum(&bad_version);
    MINILSM_CHECK(ExpectManifestCorruption(bad_version));

    std::string bad_checksum = valid;
    bad_checksum.back() ^= 0x01;
    MINILSM_CHECK(ExpectManifestCorruption(bad_checksum));

    for (std::size_t removed = 1U; removed <= 8U; ++removed) {
        MINILSM_CHECK(ExpectManifestCorruption(
            valid.substr(0U, valid.size() - removed)));
    }
    MINILSM_CHECK(ExpectManifestCorruption(valid, 9U));
    return true;
}

bool TestManifestRejectsInvalidRecords() {
    std::string valid;
    MINILSM_CHECK(
        minilsm::EncodeManifestSnapshot(ValidVersion(), &valid).ok());

    // The first record begins at byte 56: u32 record size, then level.
    std::string invalid_level = valid;
    StoreFixed32At(&invalid_level, 60U, 4U);
    RefreshManifestChecksum(&invalid_level);
    MINILSM_CHECK(ExpectManifestCorruption(invalid_level));

    std::string too_small = valid;
    StoreFixed64At(
        &too_small, 72U, minilsm::kMinSstableFileBytes - 1U);
    RefreshManifestChecksum(&too_small);
    MINILSM_CHECK(ExpectManifestCorruption(too_small));

    std::string bad_range = valid;
    bad_range[112U] = 'z';
    bad_range[113U] = 'a';
    RefreshManifestChecksum(&bad_range);
    MINILSM_CHECK(ExpectManifestCorruption(bad_range));

    std::string bad_record_size = valid;
    StoreFixed32At(&bad_record_size, 56U, 51U);
    RefreshManifestChecksum(&bad_record_size);
    MINILSM_CHECK(ExpectManifestCorruption(bad_record_size));

    std::string bad_next = valid;
    StoreFixed64At(&bad_next, 40U, 15U);
    RefreshManifestChecksum(&bad_next);
    MINILSM_CHECK(ExpectManifestCorruption(bad_next));
    return true;
}

bool TestDuplicateNumbersOverlapAndAllocatorOverflow() {
    minilsm::Version duplicate = ValidVersion();
    duplicate.levels[0][1].file_number = 12U;
    MINILSM_CHECK(
        minilsm::ValidateVersionMetadata(duplicate).code() ==
        minilsm::Status::Code::kCorruption);

    std::string encoded;
    MINILSM_CHECK(
        minilsm::EncodeManifestSnapshot(ValidVersion(), &encoded).ok());
    // Every test record here has two one-byte bounds, so the second record
    // starts at 114 and its file number starts eight bytes later.
    StoreFixed64At(&encoded, 122U, 12U);
    RefreshManifestChecksum(&encoded);
    MINILSM_CHECK(ExpectManifestCorruption(encoded));

    minilsm::Version overlaps = ValidVersion();
    overlaps.levels[1][1].smallest_user_key = "g";
    MINILSM_CHECK(
        minilsm::ValidateVersionMetadata(overlaps).code() ==
        minilsm::Status::Code::kCorruption);

    minilsm::Version version = ValidVersion();
    std::uint64_t allocated = 0U;
    MINILSM_CHECK(minilsm::AllocateFileNumber(&version, &allocated).ok());
    MINILSM_CHECK(allocated == 30U);
    MINILSM_CHECK(version.next_file_number == 31U);
    version.next_file_number =
        std::numeric_limits<std::uint64_t>::max();
    MINILSM_CHECK(
        minilsm::AllocateFileNumber(&version, &allocated).code() ==
        minilsm::Status::Code::kIOError);
    return true;
}

}  // namespace

int main() {
    MINILSM_RUN(TestManifestRoundTripAndMinimumSstableSize);
    MINILSM_RUN(TestCurrentRoundTripAndStrictGrammar);
    MINILSM_RUN(TestManifestHeaderChecksumAndTruncation);
    MINILSM_RUN(TestManifestRejectsInvalidRecords);
    MINILSM_RUN(TestDuplicateNumbersOverlapAndAllocatorOverflow);
    return 0;
}
