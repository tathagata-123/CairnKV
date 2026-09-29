#include "manifest/manifest.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "common/checksum.hpp"
#include "common/coding.hpp"
#include "common/file_names.hpp"
#include "minilsm/format_limits.hpp"

namespace minilsm {

namespace {

constexpr std::string_view kManifestMagic = "MLSMMAN1";
constexpr std::uint32_t kManifestFormatVersion = 1U;
constexpr std::uint32_t kManifestHeaderBytes = 56U;
constexpr std::uint32_t kManifestRecordFixedBytes = 52U;
constexpr std::size_t kMinimumManifestBytes = 60U;

Status AppendManifestRecord(
    const FileMetaData& file,
    std::string* encoded) {
    const std::size_t key_bytes =
        file.smallest_user_key.size() + file.largest_user_key.size();
    if (key_bytes >
        std::numeric_limits<std::uint32_t>::max() -
            kManifestRecordFixedBytes) {
        return Status::InvalidArgument(
            "MANIFEST record size overflows uint32");
    }

    PutFixed32(
        encoded,
        kManifestRecordFixedBytes +
            static_cast<std::uint32_t>(key_bytes));
    PutFixed32(encoded, file.level);
    PutFixed64(encoded, file.file_number);
    PutFixed64(encoded, file.file_size);
    PutFixed64(encoded, file.entry_count);
    PutFixed64(encoded, file.smallest_sequence);
    PutFixed64(encoded, file.largest_sequence);
    PutFixed32(
        encoded,
        static_cast<std::uint32_t>(file.smallest_user_key.size()));
    PutFixed32(
        encoded,
        static_cast<std::uint32_t>(file.largest_user_key.size()));
    encoded->append(file.smallest_user_key);
    encoded->append(file.largest_user_key);
    return Status::Ok();
}

Status DecodeManifestRecord(
    std::string_view record,
    FileMetaData* file) {
    FileMetaData decoded;
    std::uint32_t smallest_length = 0U;
    std::uint32_t largest_length = 0U;
    Status status = GetFixed32(&record, &decoded.level);
    if (status.ok()) {
        status = GetFixed64(&record, &decoded.file_number);
    }
    if (status.ok()) {
        status = GetFixed64(&record, &decoded.file_size);
    }
    if (status.ok()) {
        status = GetFixed64(&record, &decoded.entry_count);
    }
    if (status.ok()) {
        status = GetFixed64(&record, &decoded.smallest_sequence);
    }
    if (status.ok()) {
        status = GetFixed64(&record, &decoded.largest_sequence);
    }
    if (status.ok()) {
        status = GetFixed32(&record, &smallest_length);
    }
    if (status.ok()) {
        status = GetFixed32(&record, &largest_length);
    }
    if (!status.ok()) {
        return Status::Corruption("MANIFEST record fields are truncated");
    }
    if (smallest_length > kMaxUserKeyBytes ||
        largest_length > kMaxUserKeyBytes) {
        return Status::Corruption(
            "MANIFEST record user-key length exceeds 1 MiB");
    }
    const std::size_t total_key_bytes =
        static_cast<std::size_t>(smallest_length) + largest_length;
    if (record.size() != total_key_bytes) {
        return Status::Corruption(
            "MANIFEST record size does not match its key lengths");
    }

    decoded.smallest_user_key.assign(record.substr(0U, smallest_length));
    record.remove_prefix(smallest_length);
    decoded.largest_user_key.assign(record);
    *file = std::move(decoded);
    return Status::Ok();
}

}  // namespace

Status EncodeManifestSnapshot(
    const Version& version,
    std::string* encoded) {
    if (encoded == nullptr) {
        return Status::InvalidArgument(
            "MANIFEST encoder received a null output");
    }
    Status status = ValidateVersionMetadata(version);
    if (!status.ok()) {
        return Status::InvalidArgument(status.message());
    }

    std::size_t file_count = 0U;
    for (const std::vector<FileMetaData>& level : version.levels) {
        file_count += level.size();
    }

    std::string result(kManifestMagic);
    const bool two_wals = version.immutable_wal_file_number != 0U;
    PutFixed32(&result, two_wals ? 2U : kManifestFormatVersion);
    PutFixed32(&result, kManifestHeaderBytes + (two_wals ? 8U : 0U));
    PutFixed64(&result, version.manifest_file_number);
    PutFixed64(&result, version.active_wal_file_number);
    PutFixed64(&result, version.last_sequence);
    PutFixed64(&result, version.next_file_number);
    PutFixed32(&result, static_cast<std::uint32_t>(kLevelCount));
    PutFixed32(&result, static_cast<std::uint32_t>(file_count));
    if (two_wals) PutFixed64(&result, version.immutable_wal_file_number);

    for (const std::vector<FileMetaData>& level : version.levels) {
        for (const FileMetaData& file : level) {
            status = AppendManifestRecord(file, &result);
            if (!status.ok()) {
                return status;
            }
            if (result.size() > kMaxManifestFileBytes - 4U) {
                return Status::InvalidArgument(
                    "MANIFEST snapshot exceeds 64 MiB");
            }
        }
    }
    PutFixed32(&result, Crc32c(result));
    *encoded = std::move(result);
    return Status::Ok();
}

Status DecodeManifestSnapshot(
    std::string_view encoded,
    std::uint64_t expected_manifest_file_number,
    Version* version) {
    if (version == nullptr || expected_manifest_file_number == 0U) {
        return Status::InvalidArgument(
            "MANIFEST decoder received an invalid argument");
    }
    if (encoded.size() < kMinimumManifestBytes ||
        encoded.size() > kMaxManifestFileBytes) {
        return Status::Corruption(
            "MANIFEST file size is outside version 1 bounds");
    }

    std::string_view checksum_field = encoded.substr(encoded.size() - 4U);
    std::uint32_t stored_checksum = 0U;
    Status status = GetFixed32(&checksum_field, &stored_checksum);
    if (!status.ok() ||
        !VerifyCrc32c(
            encoded.substr(0U, encoded.size() - 4U), stored_checksum)) {
        return Status::Corruption("MANIFEST checksum mismatch");
    }

    std::string_view fields = encoded.substr(0U, encoded.size() - 4U);
    if (fields.substr(0U, kManifestMagic.size()) != kManifestMagic) {
        return Status::Corruption("MANIFEST magic is invalid");
    }
    fields.remove_prefix(kManifestMagic.size());

    Version decoded;
    std::uint32_t format_version = 0U;
    std::uint32_t header_size = 0U;
    std::uint32_t level_count = 0U;
    std::uint32_t file_count = 0U;
    status = GetFixed32(&fields, &format_version);
    if (status.ok()) {
        status = GetFixed32(&fields, &header_size);
    }
    if (status.ok()) {
        status = GetFixed64(&fields, &decoded.manifest_file_number);
    }
    if (status.ok()) {
        status = GetFixed64(&fields, &decoded.active_wal_file_number);
    }
    if (status.ok()) {
        status = GetFixed64(&fields, &decoded.last_sequence);
    }
    if (status.ok()) {
        status = GetFixed64(&fields, &decoded.next_file_number);
    }
    if (status.ok()) {
        status = GetFixed32(&fields, &level_count);
    }
    if (status.ok()) {
        status = GetFixed32(&fields, &file_count);
    }
    if (status.ok() && format_version == 2U) {
        status = GetFixed64(&fields, &decoded.immutable_wal_file_number);
    }
    if (!status.ok() || (format_version != 1U && format_version != 2U) ||
        header_size != kManifestHeaderBytes + (format_version == 2U ? 8U : 0U) || level_count != kLevelCount ||
        decoded.manifest_file_number != expected_manifest_file_number ||
        file_count > kMaxManifestLiveFiles) {
        return Status::Corruption("MANIFEST header fields are invalid");
    }
    if (file_count > fields.size() / (4U + kManifestRecordFixedBytes)) {
        return Status::Corruption(
            "MANIFEST file count exceeds its bounded contents");
    }

    std::uint32_t previous_level = 0U;
    bool has_previous_level = false;
    for (std::uint32_t index = 0U; index < file_count; ++index) {
        std::uint32_t record_size = 0U;
        status = GetFixed32(&fields, &record_size);
        const std::uint32_t maximum_record_size =
            kManifestRecordFixedBytes + 2U * kMaxUserKeyBytes;
        if (!status.ok() || record_size < kManifestRecordFixedBytes ||
            record_size > maximum_record_size ||
            fields.size() < record_size) {
            return Status::Corruption(
                "MANIFEST live-file record size is invalid");
        }

        FileMetaData file;
        status = DecodeManifestRecord(
            fields.substr(0U, record_size), &file);
        if (!status.ok()) {
            return status;
        }
        fields.remove_prefix(record_size);
        if (file.level >= kLevelCount) {
            return Status::Corruption(
                "MANIFEST live-file level is invalid");
        }
        if (has_previous_level && file.level < previous_level) {
            return Status::Corruption(
                "MANIFEST records are not ordered by ascending level");
        }
        previous_level = file.level;
        has_previous_level = true;
        decoded.levels[file.level].push_back(std::move(file));
    }
    if (!fields.empty()) {
        return Status::Corruption(
            "MANIFEST has trailing bytes after its records");
    }

    status = ValidateVersionMetadata(decoded);
    if (!status.ok()) {
        return status;
    }
    *version = std::move(decoded);
    return Status::Ok();
}

Status EncodeCurrent(
    std::uint64_t manifest_file_number,
    std::string* encoded) {
    if (encoded == nullptr) {
        return Status::InvalidArgument(
            "CURRENT encoder received a null output");
    }
    std::string manifest_name;
    Status status = MakeManifestFileName(
        manifest_file_number, false, &manifest_name);
    if (!status.ok()) {
        return status;
    }
    manifest_name.push_back('\n');
    *encoded = std::move(manifest_name);
    return Status::Ok();
}

Status DecodeCurrent(
    std::string_view encoded,
    std::uint64_t* manifest_file_number) {
    if (manifest_file_number == nullptr) {
        return Status::InvalidArgument(
            "CURRENT decoder received a null output");
    }
    if (encoded.size() != 30U || encoded.back() != '\n') {
        return Status::Corruption(
            "CURRENT is not exactly one 30-byte selector line");
    }

    ParsedFileName parsed;
    const Status status = ParseFileName(
        encoded.substr(0U, encoded.size() - 1U), &parsed);
    if (!status.ok() || parsed.kind != FileKind::kManifest ||
        parsed.temporary) {
        return Status::Corruption(
            "CURRENT does not name a final MANIFEST file");
    }
    *manifest_file_number = parsed.file_number;
    return Status::Ok();
}

}  // namespace minilsm
