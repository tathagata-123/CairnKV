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
#include "common/internal_key.hpp"
#include "minilsm/status.hpp"
#include "sstable/sstable_builder.hpp"
#include "sstable/sstable_format.hpp"
#include "sstable/sstable_reader.hpp"
#include "test_harness.hpp"

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern =
            (std::filesystem::temp_directory_path() /
             "minilsm-sstable-corruption-XXXXXX")
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

minilsm::InternalKey Key(std::string user_key, std::uint64_t sequence) {
    return minilsm::InternalKey{
        std::move(user_key), sequence, minilsm::RecordType::kValue};
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

std::uint32_t LoadFixed32(std::string_view bytes, std::size_t offset) {
    std::string_view input = bytes.substr(offset, 4U);
    std::uint32_t value = 0U;
    minilsm::GetFixed32(&input, &value);
    return value;
}

void RefreshFooterChecksum(std::string* bytes) {
    const std::size_t footer_offset =
        bytes->size() - minilsm::kSstableFooterBytes;
    StoreFixed32At(
        bytes,
        footer_offset + 60U,
        minilsm::Crc32c(
            std::string_view(*bytes).substr(footer_offset, 60U)));
}

void RefreshFramedBlockChecksum(
    std::string* bytes,
    std::size_t offset,
    std::size_t size) {
    StoreFixed32At(
        bytes,
        offset + size - 4U,
        minilsm::Crc32c(
            std::string_view(*bytes).substr(offset, size - 4U)));
}

bool WriteFile(
    const std::filesystem::path& path,
    std::string_view bytes) {
    std::unique_ptr<minilsm::SequentialFileWriter> writer;
    MINILSM_CHECK(
        minilsm::SequentialFileWriter::CreateNew(path, &writer).ok());
    MINILSM_CHECK(writer->Append(bytes).ok());
    MINILSM_CHECK(writer->Close().ok());
    return true;
}

bool ReadFile(
    const std::filesystem::path& path,
    std::string* bytes) {
    std::uint64_t size = 0U;
    MINILSM_CHECK(minilsm::GetFileSize(path, &size).ok());
    std::unique_ptr<minilsm::RandomAccessFileReader> reader;
    MINILSM_CHECK(
        minilsm::RandomAccessFileReader::Open(path, &reader).ok());
    MINILSM_CHECK(
        reader->Read(0U, static_cast<std::size_t>(size), bytes).ok());
    MINILSM_CHECK(reader->Close().ok());
    return true;
}

bool BuildBaseTable(
    const std::filesystem::path& path,
    std::string* bytes,
    minilsm::SstableFooter* footer) {
    std::unique_ptr<minilsm::SstableBuilder> builder;
    MINILSM_CHECK(
        minilsm::SstableBuilder::Create(path, 1024U, &builder).ok());
    MINILSM_CHECK(builder->Add(Key("alpha", 3U), "one").ok());
    MINILSM_CHECK(builder->Add(Key("omega", 2U), "two").ok());
    MINILSM_CHECK(builder->Finish().ok());

    std::unique_ptr<minilsm::SstableReader> reader;
    MINILSM_CHECK(minilsm::SstableReader::Open(path, &reader).ok());
    *footer = reader->footer();
    MINILSM_CHECK(ReadFile(path, bytes));
    return true;
}

bool ExpectOpenCorruption(const std::filesystem::path& path) {
    std::unique_ptr<minilsm::SstableReader> reader;
    MINILSM_CHECK(
        minilsm::SstableReader::Open(path, &reader).code() ==
        minilsm::Status::Code::kCorruption);
    MINILSM_CHECK(reader == nullptr);
    return true;
}

bool TestFooterCorruptionAndTruncation() {
    TemporaryDirectory directory;
    std::string valid;
    minilsm::SstableFooter footer;
    MINILSM_CHECK(BuildBaseTable(
        directory.path() / "base.sst", &valid, &footer));
    const std::size_t footer_offset =
        valid.size() - minilsm::kSstableFooterBytes;

    std::vector<std::string> corruptions;

    std::string bad_magic = valid;
    bad_magic[footer_offset] = 'X';
    RefreshFooterChecksum(&bad_magic);
    corruptions.push_back(std::move(bad_magic));

    std::string bad_version = valid;
    StoreFixed32At(&bad_version, footer_offset + 8U, 2U);
    RefreshFooterChecksum(&bad_version);
    corruptions.push_back(std::move(bad_version));

    std::string bad_checksum = valid;
    bad_checksum.back() ^= 0x01;
    corruptions.push_back(std::move(bad_checksum));

    std::string populated_filter = valid;
    StoreFixed64At(&populated_filter, footer_offset + 16U, 1U);
    StoreFixed64At(&populated_filter, footer_offset + 24U, 48U);
    RefreshFooterChecksum(&populated_filter);
    corruptions.push_back(std::move(populated_filter));

    for (std::size_t index = 0U; index < corruptions.size(); ++index) {
        const std::filesystem::path path =
            directory.path() / ("footer-" + std::to_string(index));
        MINILSM_CHECK(WriteFile(path, corruptions[index]));
        MINILSM_CHECK(ExpectOpenCorruption(path));
    }

    const std::filesystem::path truncated = directory.path() / "truncated";
    MINILSM_CHECK(WriteFile(truncated, valid.substr(0U, valid.size() - 1U)));
    MINILSM_CHECK(ExpectOpenCorruption(truncated));
    return true;
}

bool TestIndexOffsetSizeAndOverflowValidation() {
    TemporaryDirectory directory;
    std::string valid;
    minilsm::SstableFooter footer;
    MINILSM_CHECK(BuildBaseTable(
        directory.path() / "base.sst", &valid, &footer));
    const std::size_t footer_offset =
        valid.size() - minilsm::kSstableFooterBytes;

    std::vector<std::string> corruptions;

    std::string offset_outside = valid;
    StoreFixed64At(
        &offset_outside,
        footer_offset + 32U,
        static_cast<std::uint64_t>(valid.size()) + 1U);
    RefreshFooterChecksum(&offset_outside);
    corruptions.push_back(std::move(offset_outside));

    std::string size_outside = valid;
    StoreFixed64At(
        &size_outside,
        footer_offset + 40U,
        static_cast<std::uint64_t>(valid.size()));
    RefreshFooterChecksum(&size_outside);
    corruptions.push_back(std::move(size_outside));

    std::string addition_overflow = valid;
    StoreFixed64At(
        &addition_overflow,
        footer_offset + 32U,
        std::numeric_limits<std::uint64_t>::max() - 20U);
    StoreFixed64At(&addition_overflow, footer_offset + 40U, 49U);
    RefreshFooterChecksum(&addition_overflow);
    corruptions.push_back(std::move(addition_overflow));

    for (std::size_t index = 0U; index < corruptions.size(); ++index) {
        const std::filesystem::path path =
            directory.path() / ("offset-" + std::to_string(index));
        MINILSM_CHECK(WriteFile(path, corruptions[index]));
        MINILSM_CHECK(ExpectOpenCorruption(path));
    }
    return true;
}

bool TestTruncatedMalformedAndBadChecksumIndex() {
    TemporaryDirectory directory;
    std::string valid;
    minilsm::SstableFooter footer;
    MINILSM_CHECK(BuildBaseTable(
        directory.path() / "base.sst", &valid, &footer));
    const std::size_t index_offset =
        static_cast<std::size_t>(footer.index_offset);
    const std::size_t index_size =
        static_cast<std::size_t>(footer.index_size);

    std::string truncated = valid;
    const std::uint32_t body_size = LoadFixed32(truncated, index_offset);
    StoreFixed32At(&truncated, index_offset, body_size + 1U);
    const std::filesystem::path truncated_path =
        directory.path() / "truncated-index";
    MINILSM_CHECK(WriteFile(truncated_path, truncated));
    MINILSM_CHECK(ExpectOpenCorruption(truncated_path));

    std::string malformed = valid;
    StoreFixed32At(&malformed, index_offset + 16U, 1000U);
    RefreshFramedBlockChecksum(&malformed, index_offset, index_size);
    const std::filesystem::path malformed_path =
        directory.path() / "malformed-index";
    MINILSM_CHECK(WriteFile(malformed_path, malformed));
    MINILSM_CHECK(ExpectOpenCorruption(malformed_path));

    std::string bad_checksum = valid;
    bad_checksum[index_offset + index_size - 1U] ^= 0x01;
    const std::filesystem::path checksum_path =
        directory.path() / "bad-index-checksum";
    MINILSM_CHECK(WriteFile(checksum_path, bad_checksum));
    MINILSM_CHECK(ExpectOpenCorruption(checksum_path));

    std::string bad_handle = valid;
    std::string encoded_key;
    MINILSM_CHECK(minilsm::EncodeInternalKey(Key("omega", 2U), &encoded_key).ok());
    const std::size_t block_offset_field =
        index_offset + 4U + 12U + 4U + encoded_key.size();
    StoreFixed64At(&bad_handle, block_offset_field, 1U);
    RefreshFramedBlockChecksum(&bad_handle, index_offset, index_size);
    const std::filesystem::path handle_path =
        directory.path() / "bad-block-handle";
    MINILSM_CHECK(WriteFile(handle_path, bad_handle));
    MINILSM_CHECK(ExpectOpenCorruption(handle_path));
    return true;
}

bool TestFilterMetadataAndChecksumCorruption() {
    TemporaryDirectory directory;
    std::string valid;
    minilsm::SstableFooter footer;
    MINILSM_CHECK(BuildBaseTable(
        directory.path() / "base.sst", &valid, &footer));
    MINILSM_CHECK(footer.filter_offset != 0U);
    MINILSM_CHECK(footer.filter_size >=
                   minilsm::kMinBloomFilterPhysicalBytes);

    std::string bad_checksum = valid;
    bad_checksum[static_cast<std::size_t>(footer.filter_offset) + 36U] ^=
        0x01;
    const std::filesystem::path checksum_path =
        directory.path() / "bad-filter-checksum";
    MINILSM_CHECK(WriteFile(checksum_path, bad_checksum));
    MINILSM_CHECK(ExpectOpenCorruption(checksum_path));

    std::string bad_bits_per_key = valid;
    const std::size_t filter_offset =
        static_cast<std::size_t>(footer.filter_offset);
    StoreFixed32At(&bad_bits_per_key, filter_offset + 12U, 9U);
    RefreshFramedBlockChecksum(
        &bad_bits_per_key,
        filter_offset,
        static_cast<std::size_t>(footer.filter_size));
    const std::filesystem::path metadata_path =
        directory.path() / "bad-filter-metadata";
    MINILSM_CHECK(WriteFile(metadata_path, bad_bits_per_key));
    MINILSM_CHECK(ExpectOpenCorruption(metadata_path));

    std::string too_many_keys = valid;
    StoreFixed64At(&too_many_keys, filter_offset + 28U, 3U);
    RefreshFramedBlockChecksum(
        &too_many_keys,
        filter_offset,
        static_cast<std::size_t>(footer.filter_size));
    const std::filesystem::path key_count_path =
        directory.path() / "filter-key-count";
    MINILSM_CHECK(WriteFile(key_count_path, too_many_keys));
    MINILSM_CHECK(ExpectOpenCorruption(key_count_path));

    // A checksum-valid bit array cannot be judged from filter metadata alone.
    // Full live-file validation cross-checks every stored user key and rejects
    // this state before a DB can trust its definite-negative answers.
    std::string false_negative = valid;
    const std::size_t bits_begin = filter_offset + 36U;
    const std::size_t bits_end =
        filter_offset + static_cast<std::size_t>(footer.filter_size) - 4U;
    false_negative.replace(
        bits_begin, bits_end - bits_begin, bits_end - bits_begin, '\0');
    RefreshFramedBlockChecksum(
        &false_negative,
        filter_offset,
        static_cast<std::size_t>(footer.filter_size));
    const std::filesystem::path false_negative_path =
        directory.path() / "filter-false-negative";
    MINILSM_CHECK(WriteFile(false_negative_path, false_negative));
    std::unique_ptr<minilsm::SstableReader> reader;
    MINILSM_CHECK(
        minilsm::SstableReader::Open(false_negative_path, &reader).ok());
    minilsm::SstableProperties properties;
    MINILSM_CHECK(
        reader->ValidateAndReadProperties(&properties).code() ==
        minilsm::Status::Code::kCorruption);

    std::string half_absent = valid;
    const std::size_t footer_offset =
        half_absent.size() - minilsm::kSstableFooterBytes;
    StoreFixed64At(&half_absent, footer_offset + 16U, 0U);
    RefreshFooterChecksum(&half_absent);
    const std::filesystem::path half_absent_path =
        directory.path() / "half-absent-filter";
    MINILSM_CHECK(WriteFile(half_absent_path, half_absent));
    MINILSM_CHECK(ExpectOpenCorruption(half_absent_path));
    return true;
}

bool TestTruncatedDataBlockAndBadDataChecksum() {
    TemporaryDirectory directory;
    std::string valid;
    minilsm::SstableFooter footer;
    MINILSM_CHECK(BuildBaseTable(
        directory.path() / "base.sst", &valid, &footer));

    std::string truncated_block = valid;
    StoreFixed32At(
        &truncated_block, 0U, LoadFixed32(truncated_block, 0U) + 1U);
    const std::filesystem::path truncated_path =
        directory.path() / "truncated-data";
    MINILSM_CHECK(WriteFile(truncated_path, truncated_block));
    std::unique_ptr<minilsm::SstableReader> reader;
    MINILSM_CHECK(minilsm::SstableReader::Open(truncated_path, &reader).ok());
    MINILSM_CHECK(
        reader->Get(Key("alpha", 3U)).status.code() ==
        minilsm::Status::Code::kCorruption);

    std::string bad_checksum = valid;
    const std::size_t data_block_size =
        static_cast<std::size_t>(footer.index_offset);
    bad_checksum[data_block_size - 1U] ^= 0x01;
    const std::filesystem::path checksum_path =
        directory.path() / "bad-data-checksum";
    MINILSM_CHECK(WriteFile(checksum_path, bad_checksum));
    MINILSM_CHECK(minilsm::SstableReader::Open(checksum_path, &reader).ok());
    MINILSM_CHECK(
        reader->Get(Key("omega", 2U)).status.code() ==
        minilsm::Status::Code::kCorruption);
    return true;
}

}  // namespace

int main() {
    MINILSM_RUN(TestFooterCorruptionAndTruncation);
    MINILSM_RUN(TestIndexOffsetSizeAndOverflowValidation);
    MINILSM_RUN(TestTruncatedMalformedAndBadChecksumIndex);
    MINILSM_RUN(TestFilterMetadataAndChecksumCorruption);
    MINILSM_RUN(TestTruncatedDataBlockAndBadDataChecksum);
    return 0;
}
