#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cstdlib>
#include <unistd.h>

#include "common/checksum.hpp"
#include "common/coding.hpp"
#include "common/file_io.hpp"
#include "common/file_names.hpp"
#include "common/internal_key.hpp"
#include "manifest/manifest.hpp"
#include "manifest/version.hpp"
#include "minilsm/db.hpp"
#include "minilsm/format_limits.hpp"
#include "minilsm/status.hpp"
#include "sstable/sstable_builder.hpp"
#include "sstable/sstable_format.hpp"
#include "sstable/sstable_reader.hpp"
#include "test_harness.hpp"
#include "legacy_options.hpp"
#include "wal/wal.hpp"
#include "wal/wal_format.hpp"

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern =
            (std::filesystem::temp_directory_path() /
             "minilsm-m8-corruption-XXXXXX")
                .string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        if (created != nullptr) {
            path_ = created;
        }
    }

    ~TemporaryDirectory() {
        if (!path_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

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

void RefreshChecksum(
    std::string* bytes,
    std::size_t offset,
    std::size_t size) {
    StoreFixed32At(
        bytes,
        offset + size - 4U,
        minilsm::Crc32c(
            std::string_view(*bytes).substr(offset, size - 4U)));
}

bool WriteNewFile(
    const std::filesystem::path& path,
    std::string_view bytes) {
    std::unique_ptr<minilsm::SequentialFileWriter> writer;
    MINILSM_CHECK(
        minilsm::SequentialFileWriter::CreateNew(path, &writer).ok());
    MINILSM_CHECK(writer->Append(bytes).ok());
    MINILSM_CHECK(writer->Sync().ok());
    MINILSM_CHECK(writer->Close().ok());
    return true;
}

bool ReplaceFile(
    const std::filesystem::path& path,
    std::string_view bytes) {
    MINILSM_CHECK(minilsm::DeleteFileIfExists(path).ok());
    return WriteNewFile(path, bytes);
}

bool ReadFile(
    const std::filesystem::path& path,
    std::string* bytes) {
    std::uint64_t file_size = 0U;
    MINILSM_CHECK(minilsm::GetFileSize(path, &file_size).ok());
    MINILSM_CHECK(
        file_size <= std::numeric_limits<std::size_t>::max());
    std::unique_ptr<minilsm::RandomAccessFileReader> reader;
    MINILSM_CHECK(
        minilsm::RandomAccessFileReader::Open(path, &reader).ok());
    MINILSM_CHECK(
        reader
            ->Read(0U, static_cast<std::size_t>(file_size), bytes)
            .ok());
    MINILSM_CHECK(reader->Close().ok());
    return true;
}

std::filesystem::path NumberedPath(
    const std::filesystem::path& directory,
    minilsm::FileKind kind,
    std::uint64_t number) {
    std::string name;
    minilsm::Status status = minilsm::Status::InvalidArgument("kind");
    if (kind == minilsm::FileKind::kWal) {
        status = minilsm::MakeWalFileName(number, false, &name);
    } else if (kind == minilsm::FileKind::kSstable) {
        status = minilsm::MakeSstableFileName(number, false, &name);
    } else if (kind == minilsm::FileKind::kManifest) {
        status = minilsm::MakeManifestFileName(number, false, &name);
    }
    if (!status.ok()) {
        return {};
    }
    return directory / name;
}

minilsm::InternalKey Key(
    std::string user_key,
    std::uint64_t sequence) {
    return minilsm::InternalKey{
        std::move(user_key), sequence, minilsm::RecordType::kValue};
}

bool TestWalHostileFramesAndHeaderMutations() {
    minilsm::WalRecord record;
    for (std::size_t size = 0U; size < 8U; ++size) {
        MINILSM_CHECK(
            minilsm::DecodeWalFrame(std::string(size, '\0'), &record)
                .code() == minilsm::Status::Code::kCorruption);
    }

    std::string valid_frame;
    MINILSM_CHECK(
        minilsm::EncodeWalFrame(Key("key", 1U), "value", &valid_frame)
            .ok());
    MINILSM_CHECK(
        minilsm::DecodeWalFrame(valid_frame, nullptr).code() ==
        minilsm::Status::Code::kInvalidArgument);

    std::string payload_mismatch = valid_frame;
    StoreFixed32At(
        &payload_mismatch, 0U, minilsm::kMaxWalPayloadBytes + 1U);
    RefreshChecksum(&payload_mismatch, 0U, payload_mismatch.size());
    MINILSM_CHECK(
        minilsm::DecodeWalFrame(payload_mismatch, &record).code() ==
        minilsm::Status::Code::kCorruption);

    std::string malformed_key_length = valid_frame;
    StoreFixed32At(
        &malformed_key_length,
        4U,
        std::numeric_limits<std::uint32_t>::max());
    RefreshChecksum(
        &malformed_key_length, 0U, malformed_key_length.size());
    MINILSM_CHECK(
        minilsm::DecodeWalFrame(malformed_key_length, &record).code() ==
        minilsm::Status::Code::kCorruption);

    std::string invalid_type = valid_frame;
    const std::size_t type_offset =
        8U + 4U + std::string_view("key").size() + 8U;
    invalid_type[type_offset] = '\x02';
    RefreshChecksum(&invalid_type, 0U, invalid_type.size());
    MINILSM_CHECK(
        minilsm::DecodeWalFrame(invalid_type, &record).code() ==
        minilsm::Status::Code::kCorruption);

    std::string payload_corruption = valid_frame;
    payload_corruption[12U] ^= 0x01;
    MINILSM_CHECK(
        minilsm::DecodeWalFrame(payload_corruption, &record).code() ==
        minilsm::Status::Code::kCorruption);

    std::string valid_header;
    MINILSM_CHECK(minilsm::EncodeWalHeader(7U, &valid_header).ok());
    std::string bad_header_version = valid_header;
    StoreFixed32At(&bad_header_version, 8U, 2U);
    RefreshChecksum(&bad_header_version, 0U, bad_header_version.size());
    MINILSM_CHECK(
        minilsm::ValidateWalHeader(bad_header_version, 7U).code() ==
        minilsm::Status::Code::kCorruption);
    valid_header[0] = 'X';
    MINILSM_CHECK(
        minilsm::ValidateWalHeader(valid_header, 7U).code() ==
        minilsm::Status::Code::kCorruption);
    return true;
}

bool BuildBaseSstable(
    const std::filesystem::path& path,
    std::string* bytes,
    minilsm::SstableFooter* footer) {
    std::unique_ptr<minilsm::SstableBuilder> builder;
    MINILSM_CHECK(
        minilsm::SstableBuilder::Create(path, 1024U, &builder).ok());
    MINILSM_CHECK(builder->Add(Key("alpha", 3U), "new").ok());
    MINILSM_CHECK(builder->Add(Key("alpha", 2U), "old").ok());
    MINILSM_CHECK(builder->Add(Key("omega", 1U), "last").ok());
    MINILSM_CHECK(builder->Finish().ok());

    std::unique_ptr<minilsm::SstableReader> reader;
    MINILSM_CHECK(minilsm::SstableReader::Open(path, &reader).ok());
    *footer = reader->footer();
    MINILSM_CHECK(reader->index().size() == 1U);
    return ReadFile(path, bytes);
}

bool ExpectSstableValidationCorruption(
    const std::filesystem::path& path) {
    std::unique_ptr<minilsm::SstableReader> reader;
    const minilsm::Status open_status =
        minilsm::SstableReader::Open(path, &reader);
    if (!open_status.ok()) {
        MINILSM_CHECK(
            open_status.code() == minilsm::Status::Code::kCorruption);
        return true;
    }
    minilsm::SstableProperties properties;
    MINILSM_CHECK(
        reader->ValidateAndReadProperties(&properties).code() ==
        minilsm::Status::Code::kCorruption);
    return true;
}

bool TestSstableHostileLengthsOffsetsAndBloomCount() {
    TemporaryDirectory directory;
    MINILSM_CHECK(!directory.path().empty());
    std::string valid;
    minilsm::SstableFooter footer;
    MINILSM_CHECK(BuildBaseSstable(
        directory.path() / "base.sst", &valid, &footer));

    const std::size_t data_block_size =
        static_cast<std::size_t>(footer.index_offset);
    const std::size_t index_offset =
        static_cast<std::size_t>(footer.index_offset);
    const std::size_t index_size =
        static_cast<std::size_t>(footer.index_size);
    const std::size_t filter_offset =
        static_cast<std::size_t>(footer.filter_offset);
    const std::size_t filter_size =
        static_cast<std::size_t>(footer.filter_size);
    const std::size_t footer_offset =
        valid.size() - minilsm::kSstableFooterBytes;

    std::string bad_data_magic = valid;
    bad_data_magic[4U] = 'X';
    RefreshChecksum(&bad_data_magic, 0U, data_block_size);
    const std::filesystem::path data_magic_path =
        directory.path() / "bad-data-magic.sst";
    MINILSM_CHECK(WriteNewFile(data_magic_path, bad_data_magic));
    MINILSM_CHECK(ExpectSstableValidationCorruption(data_magic_path));

    std::string bad_index_version = valid;
    bad_index_version[index_offset + 8U] = '\x02';
    bad_index_version[index_offset + 9U] = '\0';
    RefreshChecksum(&bad_index_version, index_offset, index_size);
    const std::filesystem::path index_version_path =
        directory.path() / "bad-index-version.sst";
    MINILSM_CHECK(WriteNewFile(index_version_path, bad_index_version));
    MINILSM_CHECK(
        ExpectSstableValidationCorruption(index_version_path));

    std::string bad_filter_magic = valid;
    bad_filter_magic[filter_offset + 4U] = 'X';
    RefreshChecksum(&bad_filter_magic, filter_offset, filter_size);
    const std::filesystem::path filter_magic_path =
        directory.path() / "bad-filter-magic.sst";
    MINILSM_CHECK(WriteNewFile(filter_magic_path, bad_filter_magic));
    MINILSM_CHECK(ExpectSstableValidationCorruption(filter_magic_path));

    std::string bad_key_length = valid;
    StoreFixed32At(
        &bad_key_length,
        16U,
        std::numeric_limits<std::uint32_t>::max());
    RefreshChecksum(&bad_key_length, 0U, data_block_size);
    const std::filesystem::path bad_key_path =
        directory.path() / "bad-key-length.sst";
    MINILSM_CHECK(WriteNewFile(bad_key_path, bad_key_length));
    MINILSM_CHECK(ExpectSstableValidationCorruption(bad_key_path));

    std::string encoded_first_key;
    MINILSM_CHECK(
        minilsm::EncodeInternalKey(Key("alpha", 3U), &encoded_first_key)
            .ok());
    std::string bad_value_length = valid;
    StoreFixed32At(
        &bad_value_length,
        20U + encoded_first_key.size(),
        std::numeric_limits<std::uint32_t>::max());
    RefreshChecksum(&bad_value_length, 0U, data_block_size);
    const std::filesystem::path bad_value_path =
        directory.path() / "bad-value-length.sst";
    MINILSM_CHECK(WriteNewFile(bad_value_path, bad_value_length));
    MINILSM_CHECK(ExpectSstableValidationCorruption(bad_value_path));

    std::string excessive_index_count = valid;
    StoreFixed32At(
        &excessive_index_count,
        index_offset + 12U,
        std::numeric_limits<std::uint32_t>::max());
    RefreshChecksum(&excessive_index_count, index_offset, index_size);
    const std::filesystem::path index_count_path =
        directory.path() / "bad-index-count.sst";
    MINILSM_CHECK(WriteNewFile(index_count_path, excessive_index_count));
    MINILSM_CHECK(ExpectSstableValidationCorruption(index_count_path));

    std::string bad_handle = valid;
    std::string encoded_largest_key;
    MINILSM_CHECK(
        minilsm::EncodeInternalKey(Key("omega", 1U), &encoded_largest_key)
            .ok());
    StoreFixed64At(
        &bad_handle,
        index_offset + 20U + encoded_largest_key.size(),
        std::numeric_limits<std::uint64_t>::max());
    RefreshChecksum(&bad_handle, index_offset, index_size);
    const std::filesystem::path handle_path =
        directory.path() / "bad-handle.sst";
    MINILSM_CHECK(WriteNewFile(handle_path, bad_handle));
    MINILSM_CHECK(ExpectSstableValidationCorruption(handle_path));

    // One and two distinct keys both use the minimum 64-bit filter. This
    // mutation remains internally well-sized and retains all original bits,
    // so only an exact cross-check against stored user keys can reject it.
    std::string wrong_distinct_count = valid;
    StoreFixed64At(&wrong_distinct_count, filter_offset + 28U, 1U);
    RefreshChecksum(&wrong_distinct_count, filter_offset, filter_size);
    const std::filesystem::path count_path =
        directory.path() / "wrong-distinct-count.sst";
    MINILSM_CHECK(WriteNewFile(count_path, wrong_distinct_count));
    MINILSM_CHECK(ExpectSstableValidationCorruption(count_path));

    std::string bad_filter_checksum = valid;
    bad_filter_checksum[filter_offset + filter_size - 1U] ^= 0x01;
    const std::filesystem::path filter_path =
        directory.path() / "bad-filter-checksum.sst";
    MINILSM_CHECK(WriteNewFile(filter_path, bad_filter_checksum));
    MINILSM_CHECK(ExpectSstableValidationCorruption(filter_path));

    std::string overflowing_footer = valid;
    StoreFixed64At(
        &overflowing_footer,
        footer_offset + 32U,
        std::numeric_limits<std::uint64_t>::max());
    RefreshChecksum(
        &overflowing_footer,
        footer_offset,
        minilsm::kSstableFooterBytes);
    const std::filesystem::path footer_path =
        directory.path() / "overflowing-footer.sst";
    MINILSM_CHECK(WriteNewFile(footer_path, overflowing_footer));
    MINILSM_CHECK(ExpectSstableValidationCorruption(footer_path));

    std::string wrong_footer_entry_count = valid;
    StoreFixed64At(&wrong_footer_entry_count, footer_offset + 48U, 4U);
    RefreshChecksum(
        &wrong_footer_entry_count,
        footer_offset,
        minilsm::kSstableFooterBytes);
    const std::filesystem::path footer_count_path =
        directory.path() / "wrong-footer-entry-count.sst";
    MINILSM_CHECK(WriteNewFile(
        footer_count_path, wrong_footer_entry_count));
    MINILSM_CHECK(
        ExpectSstableValidationCorruption(footer_count_path));
    return true;
}

minilsm::FileMetaData ManifestFile(
    std::uint64_t number,
    std::string smallest,
    std::string largest,
    std::uint64_t smallest_sequence,
    std::uint64_t largest_sequence) {
    minilsm::FileMetaData file;
    file.file_number = number;
    file.level = 0U;
    file.file_size = minilsm::kMinSstableFileBytes;
    file.entry_count = 2U;
    file.smallest_sequence = smallest_sequence;
    file.largest_sequence = largest_sequence;
    file.smallest_user_key = std::move(smallest);
    file.largest_user_key = std::move(largest);
    return file;
}

minilsm::Version ManifestVersion() {
    minilsm::Version version;
    version.manifest_file_number = 10U;
    version.active_wal_file_number = 11U;
    version.last_sequence = 4U;
    version.next_file_number = 20U;
    version.levels[0].push_back(
        ManifestFile(12U, "a", "c", 1U, 2U));
    version.levels[0].push_back(
        ManifestFile(13U, "d", "f", 3U, 4U));
    return version;
}

bool ExpectManifestCorruption(std::string_view bytes) {
    minilsm::Version decoded;
    MINILSM_CHECK(
        minilsm::DecodeManifestSnapshot(bytes, 10U, &decoded).code() ==
        minilsm::Status::Code::kCorruption);
    return true;
}

bool TestManifestHostileCountsLengthsAndMetadata() {
    std::string valid;
    MINILSM_CHECK(
        minilsm::EncodeManifestSnapshot(ManifestVersion(), &valid).ok());

    std::vector<std::string> corruptions;

    std::string bad_checksum = valid;
    bad_checksum.back() ^= 0x01;
    corruptions.push_back(std::move(bad_checksum));

    std::string excessive_file_count = valid;
    StoreFixed32At(
        &excessive_file_count,
        52U,
        std::numeric_limits<std::uint32_t>::max());
    RefreshChecksum(&excessive_file_count, 0U, excessive_file_count.size());
    corruptions.push_back(std::move(excessive_file_count));

    std::string excessive_record_size = valid;
    StoreFixed32At(
        &excessive_record_size,
        56U,
        std::numeric_limits<std::uint32_t>::max());
    RefreshChecksum(
        &excessive_record_size, 0U, excessive_record_size.size());
    corruptions.push_back(std::move(excessive_record_size));

    std::string excessive_key_length = valid;
    StoreFixed32At(
        &excessive_key_length, 104U, minilsm::kMaxUserKeyBytes + 1U);
    RefreshChecksum(&excessive_key_length, 0U, excessive_key_length.size());
    corruptions.push_back(std::move(excessive_key_length));

    std::string invalid_level = valid;
    StoreFixed32At(&invalid_level, 60U, 4U);
    RefreshChecksum(&invalid_level, 0U, invalid_level.size());
    corruptions.push_back(std::move(invalid_level));

    std::string zero_active_wal = valid;
    StoreFixed64At(&zero_active_wal, 24U, 0U);
    RefreshChecksum(&zero_active_wal, 0U, zero_active_wal.size());
    corruptions.push_back(std::move(zero_active_wal));

    std::string zero_sstable_number = valid;
    StoreFixed64At(&zero_sstable_number, 64U, 0U);
    RefreshChecksum(
        &zero_sstable_number, 0U, zero_sstable_number.size());
    corruptions.push_back(std::move(zero_sstable_number));

    std::string duplicate_file_number = valid;
    StoreFixed64At(&duplicate_file_number, 122U, 12U);
    RefreshChecksum(
        &duplicate_file_number, 0U, duplicate_file_number.size());
    corruptions.push_back(std::move(duplicate_file_number));

    std::string invalid_range = valid;
    invalid_range[112U] = 'z';
    invalid_range[113U] = 'a';
    RefreshChecksum(&invalid_range, 0U, invalid_range.size());
    corruptions.push_back(std::move(invalid_range));

    std::string zero_sequence = valid;
    StoreFixed64At(&zero_sequence, 88U, 0U);
    RefreshChecksum(&zero_sequence, 0U, zero_sequence.size());
    corruptions.push_back(std::move(zero_sequence));

    std::string stale_last_sequence = valid;
    StoreFixed64At(&stale_last_sequence, 32U, 3U);
    RefreshChecksum(
        &stale_last_sequence, 0U, stale_last_sequence.size());
    corruptions.push_back(std::move(stale_last_sequence));

    std::string stale_next_file_number = valid;
    StoreFixed64At(&stale_next_file_number, 40U, 13U);
    RefreshChecksum(
        &stale_next_file_number, 0U, stale_next_file_number.size());
    corruptions.push_back(std::move(stale_next_file_number));

    for (const std::string& bytes : corruptions) {
        MINILSM_CHECK(ExpectManifestCorruption(bytes));
    }
    MINILSM_CHECK(ExpectManifestCorruption(
        std::string_view(valid).substr(0U, valid.size() - 1U)));
    return true;
}

struct InstalledFixture {
    minilsm::Version version;
    std::filesystem::path manifest_path;
    std::filesystem::path sstable_path;
};

bool CreateInstalledFixture(
    const std::filesystem::path& directory,
    InstalledFixture* fixture) {
    const std::filesystem::path sstable_path =
        NumberedPath(directory, minilsm::FileKind::kSstable, 3U);
    std::unique_ptr<minilsm::SstableBuilder> builder;
    MINILSM_CHECK(
        minilsm::SstableBuilder::Create(sstable_path, 1024U, &builder).ok());
    MINILSM_CHECK(builder->Add(Key("alpha", 2U), "one").ok());
    MINILSM_CHECK(builder->Add(Key("omega", 1U), "two").ok());
    MINILSM_CHECK(builder->Finish().ok());

    std::unique_ptr<minilsm::SstableReader> reader;
    MINILSM_CHECK(minilsm::SstableReader::Open(sstable_path, &reader).ok());
    minilsm::SstableProperties properties;
    MINILSM_CHECK(reader->ValidateAndReadProperties(&properties).ok());

    minilsm::FileMetaData metadata;
    metadata.file_number = 3U;
    metadata.level = 0U;
    metadata.file_size = properties.file_size;
    metadata.entry_count = properties.entry_count;
    metadata.smallest_sequence = properties.smallest_sequence;
    metadata.largest_sequence = properties.largest_sequence;
    metadata.smallest_user_key =
        properties.smallest_internal_key.user_key;
    metadata.largest_user_key =
        properties.largest_internal_key.user_key;

    const std::filesystem::path wal_path =
        NumberedPath(directory, minilsm::FileKind::kWal, 1U);
    std::unique_ptr<minilsm::WalWriter> wal_writer;
    MINILSM_CHECK(
        minilsm::WalWriter::CreateNew(wal_path, 1U, &wal_writer).ok());
    MINILSM_CHECK(wal_writer->Close().ok());

    minilsm::Version version;
    version.manifest_file_number = 2U;
    version.active_wal_file_number = 1U;
    version.last_sequence = properties.largest_sequence;
    version.next_file_number = 4U;
    version.levels[0].push_back(std::move(metadata));

    std::string manifest;
    MINILSM_CHECK(
        minilsm::EncodeManifestSnapshot(version, &manifest).ok());
    const std::filesystem::path manifest_path =
        NumberedPath(directory, minilsm::FileKind::kManifest, 2U);
    MINILSM_CHECK(WriteNewFile(manifest_path, manifest));

    std::string current;
    MINILSM_CHECK(minilsm::EncodeCurrent(2U, &current).ok());
    MINILSM_CHECK(WriteNewFile(directory / "CURRENT", current));

    fixture->version = std::move(version);
    fixture->manifest_path = manifest_path;
    fixture->sstable_path = sstable_path;
    return true;
}

bool RewriteManifest(const InstalledFixture& fixture) {
    std::string encoded;
    MINILSM_CHECK(
        minilsm::EncodeManifestSnapshot(fixture.version, &encoded).ok());
    return ReplaceFile(fixture.manifest_path, encoded);
}

bool ExpectDbOpenCorruption(const std::filesystem::path& directory) {
    minilsm::Options options = LegacyOptions();
    options.create_if_missing = false;
    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(
        minilsm::DB::Open(options, directory, &db).code() ==
        minilsm::Status::Code::kCorruption);
    MINILSM_CHECK(db == nullptr);
    return true;
}

bool TestCurrentAndReferencedFileCorruption() {
    {
        std::uint64_t manifest_number = 0U;
        const std::string malformed[] = {
            "",
            "MANIFEST-00000000000000000002",
            "MANIFEST-0000000000000000000x\n",
            "MANIFEST-00000000000000000000\n",
            "WAL-00000000000000000002.log\n",
        };
        for (const std::string& bytes : malformed) {
            MINILSM_CHECK(
                minilsm::DecodeCurrent(bytes, &manifest_number).code() ==
                minilsm::Status::Code::kCorruption);
        }
    }
    {
        TemporaryDirectory directory;
        std::string current;
        MINILSM_CHECK(minilsm::EncodeCurrent(99U, &current).ok());
        MINILSM_CHECK(WriteNewFile(directory.path() / "CURRENT", current));
        MINILSM_CHECK(ExpectDbOpenCorruption(directory.path()));
    }
    {
        TemporaryDirectory directory;
        InstalledFixture fixture;
        MINILSM_CHECK(CreateInstalledFixture(directory.path(), &fixture));
        MINILSM_CHECK(
            minilsm::DeleteFileIfExists(fixture.sstable_path).ok());
        MINILSM_CHECK(ExpectDbOpenCorruption(directory.path()));
    }
    {
        TemporaryDirectory directory;
        InstalledFixture fixture;
        MINILSM_CHECK(CreateInstalledFixture(directory.path(), &fixture));
        ++fixture.version.levels[0][0].file_size;
        MINILSM_CHECK(RewriteManifest(fixture));
        MINILSM_CHECK(ExpectDbOpenCorruption(directory.path()));
    }
    {
        TemporaryDirectory directory;
        InstalledFixture fixture;
        MINILSM_CHECK(CreateInstalledFixture(directory.path(), &fixture));
        ++fixture.version.levels[0][0].entry_count;
        MINILSM_CHECK(RewriteManifest(fixture));
        MINILSM_CHECK(ExpectDbOpenCorruption(directory.path()));
    }
    {
        TemporaryDirectory directory;
        InstalledFixture fixture;
        MINILSM_CHECK(CreateInstalledFixture(directory.path(), &fixture));
        fixture.version.levels[0][0].smallest_user_key = "beta";
        MINILSM_CHECK(RewriteManifest(fixture));
        MINILSM_CHECK(ExpectDbOpenCorruption(directory.path()));
    }
    {
        TemporaryDirectory directory;
        InstalledFixture fixture;
        MINILSM_CHECK(CreateInstalledFixture(directory.path(), &fixture));
        fixture.version.levels[0][0].smallest_sequence = 2U;
        MINILSM_CHECK(RewriteManifest(fixture));
        MINILSM_CHECK(ExpectDbOpenCorruption(directory.path()));
    }
    return true;
}

}  // namespace

int main() {
    MINILSM_RUN(TestWalHostileFramesAndHeaderMutations);
    MINILSM_RUN(TestSstableHostileLengthsOffsetsAndBloomCount);
    MINILSM_RUN(TestManifestHostileCountsLengthsAndMetadata);
    MINILSM_RUN(TestCurrentAndReferencedFileCorruption);
    return 0;
}
