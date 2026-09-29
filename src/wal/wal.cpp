#include "wal/wal.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "common/crash_point.hpp"
#include "common/checksum.hpp"
#include "common/coding.hpp"
#include "minilsm/format_limits.hpp"
#include "wal/wal_format.hpp"

namespace minilsm {

WalWriter::WalWriter(
    std::unique_ptr<SequentialFileWriter> file_writer,
    std::uint64_t file_size,
    std::uint64_t last_sequence)
    : file_writer_(std::move(file_writer)),
      file_size_(file_size),
      last_sequence_(last_sequence) {}

Status WalWriter::CreateNew(
    const std::filesystem::path& path,
    std::uint64_t wal_file_number,
    std::unique_ptr<WalWriter>* writer) {
    if (writer == nullptr) {
        return Status::InvalidArgument(
            "WAL writer creation received a null output");
    }

    std::string header;
    Status status = EncodeWalHeader(wal_file_number, &header);
    if (!status.ok()) {
        return status;
    }

    std::unique_ptr<SequentialFileWriter> file_writer;
    status = SequentialFileWriter::CreateNew(path, &file_writer);
    if (!status.ok()) {
        return status;
    }
    status = file_writer->Append(header);
    if (!status.ok()) {
        return status;
    }
    status = file_writer->Sync();
    if (!status.ok()) {
        return status;
    }

    writer->reset(new WalWriter(
        std::move(file_writer), kWalFileHeaderBytes, 0U));
    return Status::Ok();
}

Status WalWriter::OpenForAppend(
    const std::filesystem::path& path,
    std::uint64_t file_size,
    std::uint64_t last_sequence,
    std::unique_ptr<WalWriter>* writer) {
    if (writer == nullptr) {
        return Status::InvalidArgument(
            "WAL writer open received a null output");
    }
    if (file_size < kWalFileHeaderBytes ||
        file_size > kMaxDatabaseFileBytes) {
        return Status::Corruption(
            "WAL size is outside version 1 bounds");
    }

    std::unique_ptr<SequentialFileWriter> file_writer;
    Status status =
        SequentialFileWriter::OpenForAppend(path, &file_writer);
    if (!status.ok()) {
        return status;
    }
    writer->reset(
        new WalWriter(
            std::move(file_writer), file_size, last_sequence));
    return Status::Ok();
}

Status WalWriter::AppendAndSync(
    const InternalKey& internal_key,
    std::string_view value) {
    if (failed_) {
        return Status::IOError(
            "active WAL is unusable after an earlier write failure");
    }
    if (internal_key.sequence_number <= last_sequence_) {
        return Status::InvalidArgument(
            "WAL append sequence must increase strictly");
    }

    std::string frame;
    Status status = EncodeWalFrame(internal_key, value, &frame);
    if (!status.ok()) {
        return status;
    }
    return AppendFrame(frame, internal_key.sequence_number, true);
}

Status WalWriter::AppendBatch(
    std::uint64_t start_sequence, const WriteBatch& batch, bool sync) {
    if (failed_) return Status::IOError("active WAL is unusable after write failure");
    if (batch.empty()) return Status::Ok();
    if (start_sequence <= last_sequence_) {
        return Status::InvalidArgument("WAL batch sequences must increase strictly");
    }
    std::string frame;
    Status status = EncodeWalBatchFrame(start_sequence, batch, &frame);
    if (!status.ok()) return status;
    return AppendFrame(frame, start_sequence + batch.size() - 1U, sync);
}

Status WalWriter::AppendFrame(
    std::string_view frame, std::uint64_t last_sequence, bool sync) {
    if (frame.size() > kMaxDatabaseFileBytes - file_size_) {
        return Status::IOError("active WAL would exceed the 1 TiB file limit");
    }
    if (fail_next_append_for_testing_) {
        fail_next_append_for_testing_ = false;
        failed_ = true;
        return Status::IOError("simulated WAL append failure");
    }
    Status status = file_writer_->Append(frame);
    if (!status.ok()) {
        failed_ = true;
        return status;
    }
    file_size_ += frame.size();
    CrashPoint("wal_after_write_before_sync");
    if (sync) status = Sync();
    if (!status.ok()) return status;
    last_sequence_ = last_sequence;
    return Status::Ok();
}

Status WalWriter::Sync() {
    if (failed_) return Status::IOError("active WAL is unusable after write failure");
    if (fail_next_sync_for_testing_) {
        fail_next_sync_for_testing_ = false;
        failed_ = true;
        return Status::IOError("simulated WAL synchronization failure");
    }
    const Status status = file_writer_->Sync();
    if (!status.ok()) failed_ = true;
    return status;
}

Status WalWriter::TruncateTo(std::uint64_t valid_size) {
    if (failed_) {
        return Status::IOError(
            "active WAL is unusable after an earlier write failure");
    }
    if (valid_size < kWalFileHeaderBytes ||
        valid_size > file_size_) {
        return Status::InvalidArgument(
            "WAL truncation target is outside the current file");
    }

    Status status = file_writer_->Truncate(valid_size);
    if (!status.ok()) {
        failed_ = true;
        return status;
    }
    file_size_ = valid_size;
    status = file_writer_->Sync();
    if (!status.ok()) {
        failed_ = true;
        return status;
    }
    return Status::Ok();
}

Status WalWriter::Close() {
    return file_writer_->Close();
}

void WalWriter::FailNextAppendForTesting() {
    fail_next_append_for_testing_ = true;
}

void WalWriter::FailNextSyncForTesting() {
    fail_next_sync_for_testing_ = true;
}

WalReader::WalReader(
    std::unique_ptr<RandomAccessFileReader> file_reader,
    std::uint64_t file_size)
    : file_reader_(std::move(file_reader)), file_size_(file_size) {}

Status WalReader::Open(
    const std::filesystem::path& path,
    std::uint64_t expected_wal_file_number,
    std::unique_ptr<WalReader>* reader) {
    if (reader == nullptr) {
        return Status::InvalidArgument(
            "WAL reader open received a null output");
    }
    if (expected_wal_file_number == 0U) {
        return Status::InvalidArgument(
            "expected WAL file number must be nonzero");
    }

    std::uint64_t file_size = 0U;
    Status status = GetFileSize(path, &file_size);
    if (!status.ok()) {
        return status;
    }
    if (file_size < kWalFileHeaderBytes) {
        return Status::Corruption(
            "WAL file has a missing or truncated header");
    }
    if (file_size > kMaxDatabaseFileBytes) {
        return Status::Corruption("WAL file exceeds 1 TiB");
    }

    std::unique_ptr<RandomAccessFileReader> file_reader;
    status = RandomAccessFileReader::Open(path, &file_reader);
    if (!status.ok()) {
        return status;
    }
    std::string header;
    status = file_reader->Read(
        0U, static_cast<std::size_t>(kWalFileHeaderBytes), &header);
    if (!status.ok()) {
        return status;
    }
    status = ValidateWalHeader(header, expected_wal_file_number);
    if (!status.ok()) {
        return status;
    }

    reader->reset(
        new WalReader(std::move(file_reader), file_size));
    return Status::Ok();
}

Status WalReader::ReadNext(WalRecord* record, bool* at_end) {
    if (record == nullptr || at_end == nullptr) {
        return Status::InvalidArgument(
            "WAL record read received a null output");
    }
    if (pending_index_ < pending_records_.size()) {
        *record = std::move(pending_records_[pending_index_++]);
        *at_end = false;
        return Status::Ok();
    }
    pending_records_.clear();
    pending_index_ = 0U;
    if (at_end_) {
        *at_end = true;
        return Status::Ok();
    }

    const std::uint64_t remaining = file_size_ - offset_;
    if (remaining == 0U) {
        at_end_ = true;
        *at_end = true;
        return Status::Ok();
    }
    if (remaining < 4U) {
        at_end_ = true;
        has_torn_tail_ = true;
        *at_end = true;
        return Status::Ok();
    }

    std::string payload_size_bytes;
    Status status = file_reader_->Read(
        offset_, 4U, &payload_size_bytes);
    if (!status.ok()) {
        return status;
    }
    std::string_view size_input = payload_size_bytes;
    std::uint32_t payload_size = 0U;
    status = GetFixed32(&size_input, &payload_size);
    if (!status.ok()) {
        return status;
    }
    const bool is_batch = payload_size == kWalBatchMarker;
    if (is_batch) {
        if (remaining < 12U) {
            at_end_ = has_torn_tail_ = true;
            *at_end = true;
            return Status::Ok();
        }
        std::string header;
        status = file_reader_->Read(offset_, 12U, &header);
        if (!status.ok()) return status;
        std::string_view fields = std::string_view(header).substr(4U);
        std::uint32_t checksum = 0U;
        GetFixed32(&fields, &payload_size);
        GetFixed32(&fields, &checksum);
        if (!VerifyCrc32c(std::string_view(header).substr(0U, 8U), checksum)) {
            return Status::Corruption("WAL batch header checksum mismatch");
        }
    }
    const std::uint32_t maximum = is_batch ? kMaxWalBatchBytes : kMaxWalPayloadBytes;
    if (payload_size < kMinWalPayloadBytes || payload_size > maximum) {
        return Status::Corruption("WAL record payload size is outside bounds");
    }
    const std::uint64_t frame_size =
        static_cast<std::uint64_t>(payload_size) + (is_batch ? 16U : 8U);
    if (remaining < frame_size) {
        at_end_ = true;
        has_torn_tail_ = true;
        *at_end = true;
        return Status::Ok();
    }
    if (frame_size > std::numeric_limits<std::size_t>::max()) {
        return Status::Corruption(
            "WAL record size exceeds addressable memory");
    }

    std::string frame;
    status = file_reader_->Read(
        offset_, static_cast<std::size_t>(frame_size), &frame);
    if (!status.ok()) {
        return status;
    }
    if (is_batch) {
        status = DecodeWalBatchFrame(frame, &pending_records_);
        if (!status.ok()) return status;
        if (pending_records_.front().internal_key.sequence_number <= last_sequence_) {
            pending_records_.clear();
            return Status::Corruption("WAL batch sequence numbers do not increase");
        }
        last_sequence_ = pending_records_.back().internal_key.sequence_number;
        offset_ += frame_size;
        *record = std::move(pending_records_[pending_index_++]);
        *at_end = false;
        return Status::Ok();
    }
    WalRecord decoded;
    status = DecodeWalFrame(frame, &decoded);
    if (!status.ok()) {
        return status;
    }
    if (decoded.internal_key.sequence_number <= last_sequence_) {
        return Status::Corruption(
            "WAL sequence numbers are not strictly increasing");
    }

    last_sequence_ = decoded.internal_key.sequence_number;
    offset_ += frame_size;
    *record = std::move(decoded);
    *at_end = false;
    return Status::Ok();
}

Status WalReader::Close() {
    return file_reader_->Close();
}

std::uint64_t WalReader::file_size() const {
    return file_size_;
}

std::uint64_t WalReader::valid_size() const {
    return offset_;
}

bool WalReader::has_torn_tail() const {
    return has_torn_tail_;
}

}  // namespace minilsm
