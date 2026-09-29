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
#include "minilsm/format_limits.hpp"
#include "minilsm/status.hpp"
#include "test_harness.hpp"
#include "wal/wal.hpp"

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern =
            (std::filesystem::temp_directory_path() /
             "minilsm-wal-tests-XXXXXX")
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

std::filesystem::path WalPath(
    const std::filesystem::path& directory,
    std::uint64_t file_number) {
    std::string name;
    const minilsm::Status status =
        minilsm::MakeWalFileName(file_number, false, &name);
    if (!status.ok()) {
        return {};
    }
    return directory / name;
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

std::string MakeWalHeader(std::uint64_t file_number) {
    std::string header("MLSMWAL1");
    minilsm::PutFixed32(&header, 1U);
    minilsm::PutFixed32(&header, 32U);
    minilsm::PutFixed64(&header, file_number);
    minilsm::PutFixed32(&header, 0U);
    minilsm::PutFixed32(&header, minilsm::Crc32c(header));
    return header;
}

void RefreshHeaderChecksum(std::string* header) {
    StoreFixed32At(
        header,
        28U,
        minilsm::Crc32c(std::string_view(*header).substr(0U, 28U)));
}

std::string MakeFrameFromPayload(std::string_view payload) {
    std::string frame;
    minilsm::PutFixed32(
        &frame, static_cast<std::uint32_t>(payload.size()));
    frame.append(payload);
    minilsm::PutFixed32(&frame, minilsm::Crc32c(frame));
    return frame;
}

std::string MakeFrame(
    const minilsm::InternalKey& internal_key,
    std::string_view value) {
    std::string encoded_internal_key;
    const minilsm::Status status =
        minilsm::EncodeInternalKey(
            internal_key, &encoded_internal_key);
    if (!status.ok()) {
        return {};
    }
    std::string payload;
    minilsm::PutFixed32(
        &payload,
        static_cast<std::uint32_t>(encoded_internal_key.size()));
    payload.append(encoded_internal_key);
    minilsm::PutFixed32(
        &payload, static_cast<std::uint32_t>(value.size()));
    payload.append(value);
    return MakeFrameFromPayload(payload);
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

bool ExpectRecordReadCorruption(
    const std::filesystem::path& path,
    std::uint64_t file_number) {
    std::unique_ptr<minilsm::WalReader> reader;
    MINILSM_CHECK(
        minilsm::WalReader::Open(path, file_number, &reader).ok());
    minilsm::WalRecord record;
    bool at_end = false;
    MINILSM_CHECK(
        reader->ReadNext(&record, &at_end).code() ==
        minilsm::Status::Code::kCorruption);
    return true;
}

bool TestWalRoundTrip() {
    TemporaryDirectory directory;
    MINILSM_CHECK(!directory.path().empty());
    const std::filesystem::path path = WalPath(directory.path(), 7U);
    MINILSM_CHECK(!path.empty());

    std::unique_ptr<minilsm::WalWriter> writer;
    MINILSM_CHECK(
        minilsm::WalWriter::CreateNew(path, 7U, &writer).ok());
    MINILSM_CHECK(
        writer
            ->AppendAndSync(
                minilsm::InternalKey{
                    "", 1U, minilsm::RecordType::kValue},
                "")
            .ok());
    MINILSM_CHECK(
        writer
            ->AppendAndSync(
                minilsm::InternalKey{
                    "longer-key",
                    2U,
                    minilsm::RecordType::kValue},
                "a longer value")
            .ok());
    const std::string binary_key("\0\x80\xFF", 3U);
    const std::string binary_value("\0value\xFF", 7U);
    MINILSM_CHECK(
        writer
            ->AppendAndSync(
                minilsm::InternalKey{
                    binary_key,
                    3U,
                    minilsm::RecordType::kValue},
                binary_value)
            .ok());
    MINILSM_CHECK(
        writer
            ->AppendAndSync(
                minilsm::InternalKey{
                    "deleted",
                    4U,
                    minilsm::RecordType::kTombstone},
                "")
            .ok());
    MINILSM_CHECK(writer->Close().ok());

    std::string expected_bytes = MakeWalHeader(7U);
    expected_bytes.append(MakeFrame(
        minilsm::InternalKey{
            "", 1U, minilsm::RecordType::kValue},
        ""));
    expected_bytes.append(MakeFrame(
        minilsm::InternalKey{
            "longer-key", 2U, minilsm::RecordType::kValue},
        "a longer value"));
    expected_bytes.append(MakeFrame(
        minilsm::InternalKey{
            binary_key, 3U, minilsm::RecordType::kValue},
        binary_value));
    expected_bytes.append(MakeFrame(
        minilsm::InternalKey{
            "deleted", 4U, minilsm::RecordType::kTombstone},
        ""));
    std::unique_ptr<minilsm::RandomAccessFileReader> raw_reader;
    MINILSM_CHECK(
        minilsm::RandomAccessFileReader::Open(path, &raw_reader).ok());
    std::string actual_bytes;
    MINILSM_CHECK(
        raw_reader
            ->Read(0U, expected_bytes.size(), &actual_bytes)
            .ok());
    MINILSM_CHECK(actual_bytes == expected_bytes);
    MINILSM_CHECK(raw_reader->Close().ok());

    std::unique_ptr<minilsm::WalReader> reader;
    MINILSM_CHECK(
        minilsm::WalReader::Open(path, 7U, &reader).ok());
    const std::string expected_keys[] = {
        "", "longer-key", binary_key, "deleted"};
    const std::string expected_values[] = {
        "", "a longer value", binary_value, ""};
    const minilsm::RecordType expected_types[] = {
        minilsm::RecordType::kValue,
        minilsm::RecordType::kValue,
        minilsm::RecordType::kValue,
        minilsm::RecordType::kTombstone,
    };
    for (std::size_t index = 0U; index < 4U; ++index) {
        minilsm::WalRecord record;
        bool at_end = true;
        MINILSM_CHECK(reader->ReadNext(&record, &at_end).ok());
        MINILSM_CHECK(!at_end);
        MINILSM_CHECK(
            record.internal_key.user_key == expected_keys[index]);
        MINILSM_CHECK(
            record.internal_key.sequence_number == index + 1U);
        MINILSM_CHECK(
            record.internal_key.record_type ==
            expected_types[index]);
        MINILSM_CHECK(record.value == expected_values[index]);
    }
    minilsm::WalRecord record;
    bool at_end = false;
    MINILSM_CHECK(reader->ReadNext(&record, &at_end).ok());
    MINILSM_CHECK(at_end);
    MINILSM_CHECK(!reader->has_torn_tail());
    MINILSM_CHECK(reader->valid_size() == reader->file_size());
    MINILSM_CHECK(reader->Close().ok());
    return true;
}

bool TestTruncatedFinalHeaderAndPayload() {
    const std::string first_frame = MakeFrame(
        minilsm::InternalKey{
            "first", 1U, minilsm::RecordType::kValue},
        "value");
    const std::string second_frame = MakeFrame(
        minilsm::InternalKey{
            "second", 2U, minilsm::RecordType::kValue},
        "other");

    for (int kind = 0; kind < 2; ++kind) {
        TemporaryDirectory directory;
        const std::filesystem::path path =
            WalPath(directory.path(), 1U);
        std::string bytes = MakeWalHeader(1U);
        bytes.append(first_frame);
        if (kind == 0) {
            bytes.append(second_frame.substr(0U, 2U));
        } else {
            bytes.append(
                second_frame.substr(0U, second_frame.size() - 3U));
        }
        MINILSM_CHECK(WriteFile(path, bytes));

        std::unique_ptr<minilsm::WalReader> reader;
        MINILSM_CHECK(
            minilsm::WalReader::Open(path, 1U, &reader).ok());
        minilsm::WalRecord record;
        bool at_end = false;
        MINILSM_CHECK(reader->ReadNext(&record, &at_end).ok());
        MINILSM_CHECK(!at_end);
        MINILSM_CHECK(record.internal_key.user_key == "first");
        MINILSM_CHECK(reader->ReadNext(&record, &at_end).ok());
        MINILSM_CHECK(at_end);
        MINILSM_CHECK(reader->has_torn_tail());
        MINILSM_CHECK(
            reader->valid_size() ==
            minilsm::kWalFileHeaderBytes + first_frame.size());
    }
    return true;
}

bool TestChecksumCorruptionIsNotATornTail() {
    TemporaryDirectory directory;
    const std::filesystem::path path = WalPath(directory.path(), 1U);
    std::string bytes = MakeWalHeader(1U);
    bytes.append(MakeFrame(
        minilsm::InternalKey{
            "key", 1U, minilsm::RecordType::kValue},
        "value"));
    bytes[bytes.size() - 5U] = static_cast<char>(
        static_cast<unsigned char>(bytes[bytes.size() - 5U]) ^
        0x01U);
    MINILSM_CHECK(WriteFile(path, bytes));
    MINILSM_CHECK(ExpectRecordReadCorruption(path, 1U));
    return true;
}

bool TestInvalidWalHeaders() {
    const std::size_t test_count = 6U;
    for (std::size_t index = 0U; index < test_count; ++index) {
        TemporaryDirectory directory;
        const std::filesystem::path path =
            WalPath(directory.path(), 1U);
        std::string header = MakeWalHeader(1U);
        if (index == 0U) {
            header[0] = 'X';
            RefreshHeaderChecksum(&header);
        } else if (index == 1U) {
            StoreFixed32At(&header, 8U, 2U);
            RefreshHeaderChecksum(&header);
        } else if (index == 2U) {
            StoreFixed32At(&header, 12U, 31U);
            RefreshHeaderChecksum(&header);
        } else if (index == 3U) {
            StoreFixed32At(&header, 24U, 1U);
            RefreshHeaderChecksum(&header);
        } else if (index == 4U) {
            StoreFixed64At(&header, 16U, 2U);
            RefreshHeaderChecksum(&header);
        } else {
            header[31] = static_cast<char>(
                static_cast<unsigned char>(header[31]) ^ 0x01U);
        }
        MINILSM_CHECK(WriteFile(path, header));
        std::unique_ptr<minilsm::WalReader> reader;
        MINILSM_CHECK(
            minilsm::WalReader::Open(path, 1U, &reader).code() ==
            minilsm::Status::Code::kCorruption);
    }

    TemporaryDirectory directory;
    const std::filesystem::path path = WalPath(directory.path(), 1U);
    MINILSM_CHECK(
        WriteFile(path, MakeWalHeader(1U).substr(0U, 31U)));
    std::unique_ptr<minilsm::WalReader> reader;
    MINILSM_CHECK(
        minilsm::WalReader::Open(path, 1U, &reader).code() ==
        minilsm::Status::Code::kCorruption);
    return true;
}

bool TestInvalidPayloadBoundsAndRecordMetadata() {
    {
        TemporaryDirectory directory;
        const std::filesystem::path path =
            WalPath(directory.path(), 1U);
        std::string bytes = MakeWalHeader(1U);
        minilsm::PutFixed32(
            &bytes, minilsm::kMinWalPayloadBytes - 1U);
        MINILSM_CHECK(WriteFile(path, bytes));
        MINILSM_CHECK(ExpectRecordReadCorruption(path, 1U));
    }
    {
        TemporaryDirectory directory;
        const std::filesystem::path path =
            WalPath(directory.path(), 1U);
        std::string bytes = MakeWalHeader(1U);
        minilsm::PutFixed32(
            &bytes, minilsm::kMaxWalPayloadBytes + 1U);
        MINILSM_CHECK(WriteFile(path, bytes));
        MINILSM_CHECK(ExpectRecordReadCorruption(path, 1U));
    }

    std::vector<std::string> invalid_payloads;

    std::string short_internal_key_size(21U, '\0');
    StoreFixed32At(&short_internal_key_size, 0U, 12U);
    invalid_payloads.push_back(short_internal_key_size);

    std::string large_internal_key_size(21U, '\0');
    StoreFixed32At(
        &large_internal_key_size,
        0U,
        static_cast<std::uint32_t>(
            minilsm::kMaxInternalKeyBytes + 1U));
    invalid_payloads.push_back(large_internal_key_size);

    std::string oversized_user_key_length;
    minilsm::PutFixed32(&oversized_user_key_length, 13U);
    minilsm::PutFixed32(
        &oversized_user_key_length,
        minilsm::kMaxUserKeyBytes + 1U);
    minilsm::PutFixed64(&oversized_user_key_length, 1U);
    oversized_user_key_length.push_back(
        static_cast<char>(minilsm::RecordType::kValue));
    minilsm::PutFixed32(&oversized_user_key_length, 0U);
    invalid_payloads.push_back(oversized_user_key_length);

    std::string zero_sequence;
    minilsm::PutFixed32(&zero_sequence, 13U);
    minilsm::PutFixed32(&zero_sequence, 0U);
    minilsm::PutFixed64(&zero_sequence, 0U);
    zero_sequence.push_back(
        static_cast<char>(minilsm::RecordType::kValue));
    minilsm::PutFixed32(&zero_sequence, 0U);
    invalid_payloads.push_back(zero_sequence);

    std::string invalid_type;
    minilsm::PutFixed32(&invalid_type, 13U);
    minilsm::PutFixed32(&invalid_type, 0U);
    minilsm::PutFixed64(&invalid_type, 1U);
    invalid_type.push_back('\x02');
    minilsm::PutFixed32(&invalid_type, 0U);
    invalid_payloads.push_back(invalid_type);

    std::string invalid_value_size;
    const std::string valid_internal = [] {
        std::string encoded;
        minilsm::EncodeInternalKey(
            minilsm::InternalKey{
                "", 1U, minilsm::RecordType::kValue},
            &encoded);
        return encoded;
    }();
    minilsm::PutFixed32(
        &invalid_value_size,
        static_cast<std::uint32_t>(valid_internal.size()));
    invalid_value_size.append(valid_internal);
    minilsm::PutFixed32(
        &invalid_value_size, minilsm::kMaxValueBytes + 1U);
    invalid_payloads.push_back(invalid_value_size);

    invalid_payloads.push_back([] {
        std::string encoded;
        minilsm::EncodeInternalKey(
            minilsm::InternalKey{
                "gone", 1U, minilsm::RecordType::kTombstone},
            &encoded);
        std::string payload;
        minilsm::PutFixed32(
            &payload, static_cast<std::uint32_t>(encoded.size()));
        payload.append(encoded);
        minilsm::PutFixed32(&payload, 1U);
        payload.push_back('x');
        return payload;
    }());

    for (const std::string& payload : invalid_payloads) {
        TemporaryDirectory directory;
        const std::filesystem::path path =
            WalPath(directory.path(), 1U);
        const std::string bytes =
            MakeWalHeader(1U) + MakeFrameFromPayload(payload);
        MINILSM_CHECK(WriteFile(path, bytes));
        MINILSM_CHECK(ExpectRecordReadCorruption(path, 1U));
    }
    return true;
}

bool TestSequenceOrderingAndBoundary() {
    const std::uint64_t maximum =
        std::numeric_limits<std::uint64_t>::max();
    const std::vector<std::vector<std::uint64_t>> invalid_sequences = {
        {1U, 1U},
        {2U, 1U},
        {maximum, maximum},
    };
    for (const std::vector<std::uint64_t>& sequences :
         invalid_sequences) {
        TemporaryDirectory directory;
        const std::filesystem::path path =
            WalPath(directory.path(), 1U);
        std::string bytes = MakeWalHeader(1U);
        for (const std::uint64_t sequence : sequences) {
            bytes.append(MakeFrame(
                minilsm::InternalKey{
                    "key", sequence, minilsm::RecordType::kValue},
                "value"));
        }
        MINILSM_CHECK(WriteFile(path, bytes));

        std::unique_ptr<minilsm::WalReader> reader;
        MINILSM_CHECK(
            minilsm::WalReader::Open(path, 1U, &reader).ok());
        minilsm::WalRecord record;
        bool at_end = false;
        MINILSM_CHECK(reader->ReadNext(&record, &at_end).ok());
        MINILSM_CHECK(
            reader->ReadNext(&record, &at_end).code() ==
            minilsm::Status::Code::kCorruption);
    }

    TemporaryDirectory directory;
    const std::filesystem::path path = WalPath(directory.path(), 1U);
    const std::string bytes =
        MakeWalHeader(1U) +
        MakeFrame(
            minilsm::InternalKey{
                "max", maximum, minilsm::RecordType::kValue},
            "value");
    MINILSM_CHECK(WriteFile(path, bytes));
    std::unique_ptr<minilsm::WalReader> reader;
    MINILSM_CHECK(
        minilsm::WalReader::Open(path, 1U, &reader).ok());
    minilsm::WalRecord record;
    bool at_end = false;
    MINILSM_CHECK(reader->ReadNext(&record, &at_end).ok());
    MINILSM_CHECK(record.internal_key.sequence_number == maximum);
    MINILSM_CHECK(reader->ReadNext(&record, &at_end).ok());
    MINILSM_CHECK(at_end);
    return true;
}

bool TestWriterRejectsInvalidTombstoneAndPoisonsAfterFailure() {
    TemporaryDirectory directory;
    const std::filesystem::path path = WalPath(directory.path(), 1U);
    std::unique_ptr<minilsm::WalWriter> writer;
    MINILSM_CHECK(
        minilsm::WalWriter::CreateNew(path, 1U, &writer).ok());
    MINILSM_CHECK(
        writer
            ->AppendAndSync(
                minilsm::InternalKey{
                    "bad",
                    1U,
                    minilsm::RecordType::kTombstone},
                "value")
            .code() ==
        minilsm::Status::Code::kInvalidArgument);
    MINILSM_CHECK(
        writer
            ->AppendAndSync(
                minilsm::InternalKey{
                    "first", 2U, minilsm::RecordType::kValue},
                "value")
            .ok());
    MINILSM_CHECK(
        writer
            ->AppendAndSync(
                minilsm::InternalKey{
                    "duplicate", 2U, minilsm::RecordType::kValue},
                "value")
            .code() ==
        minilsm::Status::Code::kInvalidArgument);

    writer->FailNextAppendForTesting();
    MINILSM_CHECK(
        writer
            ->AppendAndSync(
                minilsm::InternalKey{
                    "key", 3U, minilsm::RecordType::kValue},
                "value")
            .code() == minilsm::Status::Code::kIOError);
    MINILSM_CHECK(
        writer
            ->AppendAndSync(
                minilsm::InternalKey{
                    "other", 4U, minilsm::RecordType::kValue},
                "value")
            .code() == minilsm::Status::Code::kIOError);
    MINILSM_CHECK(writer->Close().ok());
    return true;
}

}  // namespace

int main() {
    MINILSM_RUN(TestWalRoundTrip);
    MINILSM_RUN(TestTruncatedFinalHeaderAndPayload);
    MINILSM_RUN(TestChecksumCorruptionIsNotATornTail);
    MINILSM_RUN(TestInvalidWalHeaders);
    MINILSM_RUN(TestInvalidPayloadBoundsAndRecordMetadata);
    MINILSM_RUN(TestSequenceOrderingAndBoundary);
    MINILSM_RUN(TestWriterRejectsInvalidTombstoneAndPoisonsAfterFailure);
    return 0;
}
