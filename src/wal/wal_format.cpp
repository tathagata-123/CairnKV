#include "wal/wal_format.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "common/checksum.hpp"
#include "common/coding.hpp"
#include "minilsm/format_limits.hpp"

namespace minilsm {

namespace {

constexpr std::string_view kWalMagic = "MLSMWAL1";
constexpr std::uint32_t kWalFormatVersion = 1U;

}  // namespace

Status EncodeWalHeader(
    std::uint64_t wal_file_number,
    std::string* header) {
    if (wal_file_number == 0U) {
        return Status::InvalidArgument(
            "WAL file number must be nonzero");
    }

    std::string result(kWalMagic);
    PutFixed32(&result, kWalFormatVersion);
    PutFixed32(
        &result, static_cast<std::uint32_t>(kWalFileHeaderBytes));
    PutFixed64(&result, wal_file_number);
    PutFixed32(&result, 0U);
    PutFixed32(&result, Crc32c(result));
    *header = std::move(result);
    return Status::Ok();
}

Status ValidateWalHeader(
    std::string_view header,
    std::uint64_t expected_wal_file_number) {
    if (header.size() != kWalFileHeaderBytes) {
        return Status::Corruption(
            "WAL header is not exactly 32 bytes");
    }
    if (header.substr(0U, kWalMagic.size()) != kWalMagic) {
        return Status::Corruption("WAL header has invalid magic");
    }

    std::string_view fields = header.substr(kWalMagic.size());
    std::uint32_t version = 0U;
    std::uint32_t header_size = 0U;
    std::uint64_t wal_file_number = 0U;
    std::uint32_t reserved = 0U;
    std::uint32_t stored_checksum = 0U;
    Status status = GetFixed32(&fields, &version);
    if (status.ok()) {
        status = GetFixed32(&fields, &header_size);
    }
    if (status.ok()) {
        status = GetFixed64(&fields, &wal_file_number);
    }
    if (status.ok()) {
        status = GetFixed32(&fields, &reserved);
    }
    if (status.ok()) {
        status = GetFixed32(&fields, &stored_checksum);
    }
    if (!status.ok() || !fields.empty()) {
        return Status::Corruption("WAL header fields are malformed");
    }
    if (version != kWalFormatVersion) {
        return Status::Corruption("WAL header has unsupported version");
    }
    if (header_size != kWalFileHeaderBytes) {
        return Status::Corruption("WAL header size is not 32");
    }
    if (wal_file_number == 0U ||
        wal_file_number != expected_wal_file_number) {
        return Status::Corruption(
            "WAL header file number does not match its name");
    }
    if (reserved != 0U) {
        return Status::Corruption(
            "WAL header reserved field is nonzero");
    }
    if (!VerifyCrc32c(
            header.substr(0U, kWalFileHeaderBytes - 4U),
            stored_checksum)) {
        return Status::Corruption("WAL header checksum mismatch");
    }
    return Status::Ok();
}

Status EncodeWalFrame(
    const InternalKey& internal_key,
    std::string_view value,
    std::string* frame) {
    if (value.size() > kMaxValueBytes) {
        return Status::InvalidArgument("WAL value exceeds 64 MiB");
    }
    if (internal_key.record_type == RecordType::kTombstone &&
        !value.empty()) {
        return Status::InvalidArgument(
            "WAL tombstone must not contain a value");
    }

    std::string encoded_internal_key;
    Status status =
        EncodeInternalKey(internal_key, &encoded_internal_key);
    if (!status.ok()) {
        return status;
    }

    const std::size_t payload_size =
        4U + encoded_internal_key.size() + 4U + value.size();
    if (payload_size < kMinWalPayloadBytes ||
        payload_size > kMaxWalPayloadBytes) {
        return Status::InvalidArgument(
            "WAL payload size is outside version 1 bounds");
    }

    std::string result;
    result.reserve(4U + payload_size + 4U);
    PutFixed32(
        &result, static_cast<std::uint32_t>(payload_size));
    PutFixed32(
        &result,
        static_cast<std::uint32_t>(encoded_internal_key.size()));
    result.append(encoded_internal_key);
    PutFixed32(
        &result, static_cast<std::uint32_t>(value.size()));
    result.append(value);
    PutFixed32(&result, Crc32c(result));
    *frame = std::move(result);
    return Status::Ok();
}

Status DecodeWalFrame(
    std::string_view frame,
    WalRecord* record) {
    if (record == nullptr) {
        return Status::InvalidArgument(
            "WAL record decoder received a null output");
    }
    if (frame.size() < 8U) {
        return Status::Corruption(
            "WAL record is shorter than its size and checksum fields");
    }

    std::string_view checksum_input =
        frame.substr(0U, frame.size() - 4U);
    std::string_view checksum_field =
        frame.substr(frame.size() - 4U);
    std::uint32_t stored_checksum = 0U;
    Status status =
        GetFixed32(&checksum_field, &stored_checksum);
    if (!status.ok() ||
        !VerifyCrc32c(checksum_input, stored_checksum)) {
        return Status::Corruption("WAL record checksum mismatch");
    }

    std::string_view fields = frame;
    std::uint32_t payload_size = 0U;
    status = GetFixed32(&fields, &payload_size);
    if (!status.ok() || payload_size < kMinWalPayloadBytes ||
        payload_size > kMaxWalPayloadBytes ||
        fields.size() != static_cast<std::size_t>(payload_size) + 4U) {
        return Status::Corruption(
            "WAL record payload size is inconsistent");
    }
    fields = fields.substr(0U, payload_size);

    std::uint32_t internal_key_size = 0U;
    status = GetFixed32(&fields, &internal_key_size);
    if (!status.ok()) {
        return Status::Corruption(
            "WAL record has a truncated InternalKey size");
    }
    if (internal_key_size < kMinInternalKeyBytes ||
        internal_key_size > kMaxInternalKeyBytes) {
        return Status::Corruption(
            "WAL record InternalKey size is outside version 1 bounds");
    }
    if (fields.size() < internal_key_size) {
        return Status::Corruption(
            "WAL record InternalKey extends beyond its payload");
    }

    const std::string_view encoded_internal_key =
        fields.substr(0U, internal_key_size);
    fields.remove_prefix(internal_key_size);
    InternalKey internal_key;
    status = DecodeInternalKey(encoded_internal_key, &internal_key);
    if (!status.ok()) {
        return Status::Corruption(
            "WAL record contains an invalid InternalKey: " +
            status.message());
    }

    std::uint32_t value_size = 0U;
    status = GetFixed32(&fields, &value_size);
    if (!status.ok()) {
        return Status::Corruption(
            "WAL record has a truncated value size");
    }
    if (value_size > kMaxValueBytes) {
        return Status::Corruption(
            "WAL record value size exceeds 64 MiB");
    }
    if (fields.size() != value_size) {
        return Status::Corruption(
            "WAL record value size is inconsistent with its payload");
    }
    if (internal_key.record_type == RecordType::kTombstone &&
        value_size != 0U) {
        return Status::Corruption(
            "WAL tombstone record contains value bytes");
    }

    *record = WalRecord{
        std::move(internal_key), std::string(fields)};
    return Status::Ok();
}

// Batch extension: marker, payload length, CRC(header's first 8 bytes), payload,
// CRC(all preceding bytes). Payload: first sequence, count, then mutations with
// type, key length, value length, key and value. All integers are little-endian.
Status EncodeWalBatchFrame(
    std::uint64_t start_sequence, const WriteBatch& batch, std::string* frame) {
    if (frame == nullptr || batch.empty() || batch.size() > kMaxWalBatchMutations ||
        start_sequence == 0U || batch.size() - 1U >
            std::numeric_limits<std::uint64_t>::max() - start_sequence) {
        return Status::InvalidArgument("invalid WAL batch count or sequence range");
    }
    std::size_t payload_size = 12U;
    for (const Mutation& mutation : batch.mutations()) {
        if (mutation.key.size() > kMaxUserKeyBytes || mutation.value.size() > kMaxValueBytes ||
            (mutation.type != MutationType::kPut && mutation.type != MutationType::kDelete) ||
            (mutation.type == MutationType::kDelete && !mutation.value.empty())) {
            return Status::InvalidArgument("invalid WAL batch mutation");
        }
        const std::size_t bytes = 9U + mutation.key.size() + mutation.value.size();
        if (bytes > kMaxWalBatchBytes - payload_size) {
            return Status::InvalidArgument("WAL batch exceeds 128 MiB");
        }
        payload_size += bytes;
    }
    std::string result;
    result.reserve(payload_size + 16U);
    PutFixed32(&result, kWalBatchMarker);
    PutFixed32(&result, static_cast<std::uint32_t>(payload_size));
    PutFixed32(&result, Crc32c(result));
    PutFixed64(&result, start_sequence);
    PutFixed32(&result, static_cast<std::uint32_t>(batch.size()));
    for (const Mutation& mutation : batch.mutations()) {
        result.push_back(static_cast<char>(mutation.type));
        PutFixed32(&result, static_cast<std::uint32_t>(mutation.key.size()));
        PutFixed32(&result, static_cast<std::uint32_t>(mutation.value.size()));
        result.append(mutation.key);
        result.append(mutation.value);
    }
    PutFixed32(&result, Crc32c(result));
    *frame = std::move(result);
    return Status::Ok();
}

Status DecodeWalBatchFrame(std::string_view frame, std::vector<WalRecord>* records) {
    if (records == nullptr) return Status::InvalidArgument("null WAL batch output");
    if (frame.size() < 37U || frame.size() > kMaxWalBatchBytes + 16U) {
        return Status::Corruption("invalid WAL batch frame size");
    }
    std::string_view input = frame;
    std::uint32_t marker = 0U, payload_size = 0U, checksum = 0U;
    GetFixed32(&input, &marker);
    GetFixed32(&input, &payload_size);
    GetFixed32(&input, &checksum);
    if (marker != kWalBatchMarker || payload_size != frame.size() - 16U ||
        !VerifyCrc32c(frame.substr(0U, 8U), checksum)) {
        return Status::Corruption("invalid WAL batch header");
    }
    std::string_view checksum_field = frame.substr(frame.size() - 4U);
    GetFixed32(&checksum_field, &checksum);
    if (!VerifyCrc32c(frame.substr(0U, frame.size() - 4U), checksum)) {
        return Status::Corruption("WAL batch checksum mismatch");
    }
    input = input.substr(0U, payload_size);
    std::uint64_t sequence = 0U;
    std::uint32_t count = 0U;
    GetFixed64(&input, &sequence);
    GetFixed32(&input, &count);
    if (count == 0U || count > kMaxWalBatchMutations || count > input.size() / 9U ||
        sequence == 0U || count - 1U > std::numeric_limits<std::uint64_t>::max() - sequence) {
        return Status::Corruption("invalid WAL batch sequence range or count");
    }
    std::vector<WalRecord> decoded;
    decoded.reserve(count);
    for (std::uint32_t i = 0U; i < count; ++i) {
        if (input.size() < 9U) return Status::Corruption("truncated WAL batch mutation");
        const auto type = static_cast<unsigned char>(input.front());
        input.remove_prefix(1U);
        std::uint32_t key_size = 0U, value_size = 0U;
        GetFixed32(&input, &key_size);
        GetFixed32(&input, &value_size);
        if (type > 1U || key_size > kMaxUserKeyBytes || value_size > kMaxValueBytes ||
            (type == 0U && value_size != 0U) || key_size > input.size() ||
            value_size > input.size() - key_size) {
            return Status::Corruption("invalid WAL batch mutation");
        }
        InternalKey key{std::string(input.substr(0U, key_size)), sequence + i,
                        static_cast<RecordType>(type)};
        input.remove_prefix(key_size);
        decoded.push_back({std::move(key), std::string(input.substr(0U, value_size))});
        input.remove_prefix(value_size);
    }
    if (!input.empty()) return Status::Corruption("trailing WAL batch bytes");
    *records = std::move(decoded); // Publish only after every mutation validates.
    return Status::Ok();
}

}  // namespace minilsm
