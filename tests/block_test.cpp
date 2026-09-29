#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "common/checksum.hpp"
#include "common/coding.hpp"
#include "common/internal_key.hpp"
#include "minilsm/status.hpp"
#include "sstable/block.hpp"
#include "test_harness.hpp"

namespace {

using minilsm::DataBlockBuilder;
using minilsm::DataBlockReader;
using minilsm::InternalKey;
using minilsm::RecordType;

InternalKey Key(
    std::string user_key,
    std::uint64_t sequence,
    RecordType type = RecordType::kValue) {
    return InternalKey{std::move(user_key), sequence, type};
}

bool KeysEqual(const InternalKey& left, const InternalKey& right) {
    return left.user_key == right.user_key &&
           left.sequence_number == right.sequence_number &&
           left.record_type == right.record_type;
}

void StoreFixed32At(
    std::string* bytes,
    std::size_t offset,
    std::uint32_t value) {
    std::string encoded;
    minilsm::PutFixed32(&encoded, value);
    bytes->replace(offset, encoded.size(), encoded);
}

void RefreshBlockChecksum(std::string* block) {
    StoreFixed32At(
        block,
        block->size() - 4U,
        minilsm::Crc32c(
            std::string_view(*block).substr(0U, block->size() - 4U)));
}

bool TestEmptyOneAndSeveralEntries() {
    DataBlockBuilder empty;
    std::string block = "unchanged";
    MINILSM_CHECK(
        empty.Finish(&block).code() ==
        minilsm::Status::Code::kInvalidArgument);
    MINILSM_CHECK(block == "unchanged");

    DataBlockBuilder builder;
    const InternalKey first = Key("alpha", 7U);
    MINILSM_CHECK(builder.Add(first, "one").ok());
    MINILSM_CHECK(builder.entry_count() == 1U);
    MINILSM_CHECK(builder.Finish(&block).ok());

    DataBlockReader reader;
    MINILSM_CHECK(DataBlockReader::Decode(block, &reader).ok());
    MINILSM_CHECK(reader.entries().size() == 1U);
    MINILSM_CHECK(KeysEqual(reader.entries()[0].internal_key, first));
    MINILSM_CHECK(reader.entries()[0].value == "one");

    DataBlockBuilder several;
    const InternalKey records[] = {
        Key("alpha", 9U),
        Key("alpha", 4U),
        Key("beta", 3U, RecordType::kTombstone),
        Key(std::string("z\0\xFF", 3U), 1U),
    };
    const std::string values[] = {
        std::string("new\0value", 9U), "old", "", "last"};
    for (std::size_t index = 0U; index < 4U; ++index) {
        MINILSM_CHECK(several.Add(records[index], values[index]).ok());
    }
    MINILSM_CHECK(several.Finish(&block).ok());
    MINILSM_CHECK(DataBlockReader::Decode(block, &reader).ok());
    MINILSM_CHECK(reader.entries().size() == 4U);
    for (std::size_t index = 0U; index < 4U; ++index) {
        MINILSM_CHECK(
            KeysEqual(reader.entries()[index].internal_key, records[index]));
        MINILSM_CHECK(reader.entries()[index].value == values[index]);
    }
    return true;
}

bool TestExactVersionOneBytes() {
    const InternalKey key = Key("a", 1U);
    DataBlockBuilder builder;
    MINILSM_CHECK(builder.Add(key, "v").ok());
    std::string actual;
    MINILSM_CHECK(builder.Finish(&actual).ok());

    std::string encoded_key;
    MINILSM_CHECK(minilsm::EncodeInternalKey(key, &encoded_key).ok());
    std::string body("MDB1");
    minilsm::PutFixed16(&body, 1U);
    minilsm::PutFixed16(&body, 0U);
    minilsm::PutFixed32(&body, 1U);
    minilsm::PutFixed32(
        &body, static_cast<std::uint32_t>(encoded_key.size()));
    body.append(encoded_key);
    minilsm::PutFixed32(&body, 1U);
    body.push_back('v');

    std::string expected;
    minilsm::PutFixed32(
        &expected, static_cast<std::uint32_t>(body.size()));
    expected.append(body);
    minilsm::PutFixed32(&expected, minilsm::Crc32c(expected));
    MINILSM_CHECK(actual == expected);
    return true;
}

bool TestExactTargetAndOversizedEntry() {
    // The framed block has 20 bytes of fixed overhead. "a" encodes to a
    // 14-byte InternalKey, so a 982-byte value makes exactly 1,024 bytes.
    DataBlockBuilder exact;
    MINILSM_CHECK(exact.Add(Key("a", 1U), std::string(982U, 'x')).ok());
    std::string block;
    MINILSM_CHECK(exact.Finish(&block).ok());
    MINILSM_CHECK(block.size() == 1024U);

    DataBlockBuilder oversized;
    MINILSM_CHECK(
        oversized.Add(Key("large", 1U), std::string(2000U, 'v')).ok());
    MINILSM_CHECK(oversized.Finish(&block).ok());
    MINILSM_CHECK(block.size() > 1024U);
    DataBlockReader reader;
    MINILSM_CHECK(DataBlockReader::Decode(block, &reader).ok());
    MINILSM_CHECK(reader.entries().size() == 1U);
    MINILSM_CHECK(reader.entries()[0].value.size() == 2000U);
    return true;
}

bool TestBuilderOrderingAndRecordValidation() {
    DataBlockBuilder builder;
    MINILSM_CHECK(builder.Add(Key("b", 3U), "value").ok());
    MINILSM_CHECK(
        builder.Add(Key("b", 3U), "duplicate").code() ==
        minilsm::Status::Code::kInvalidArgument);
    MINILSM_CHECK(
        builder.Add(Key("a", 2U), "decreasing").code() ==
        minilsm::Status::Code::kInvalidArgument);

    DataBlockBuilder same_sequence;
    MINILSM_CHECK(
        same_sequence
            .Add(Key("key", 8U, RecordType::kTombstone), "")
            .ok());
    MINILSM_CHECK(
        same_sequence.Add(Key("key", 8U), "value").code() ==
        minilsm::Status::Code::kInvalidArgument);

    DataBlockBuilder tombstone;
    MINILSM_CHECK(
        tombstone
            .Add(Key("gone", 1U, RecordType::kTombstone), "not-empty")
            .code() == minilsm::Status::Code::kInvalidArgument);
    return true;
}

bool TestMalformedTruncatedEntryAndBadChecksum() {
    DataBlockBuilder builder;
    MINILSM_CHECK(builder.Add(Key("key", 1U), "value").ok());
    std::string valid;
    MINILSM_CHECK(builder.Finish(&valid).ok());

    DataBlockReader reader;
    for (std::size_t removed = 1U; removed <= 4U; ++removed) {
        const std::string truncated =
            valid.substr(0U, valid.size() - removed);
        MINILSM_CHECK(
            DataBlockReader::Decode(truncated, &reader).code() ==
            minilsm::Status::Code::kCorruption);
    }

    std::string malformed = valid;
    // First entry InternalKey size begins after the frame size and 12-byte
    // data-block header. Keep the checksum valid to exercise length checks.
    StoreFixed32At(&malformed, 16U, 1000U);
    RefreshBlockChecksum(&malformed);
    MINILSM_CHECK(
        DataBlockReader::Decode(malformed, &reader).code() ==
        minilsm::Status::Code::kCorruption);

    std::string bad_checksum = valid;
    bad_checksum[bad_checksum.size() - 1U] ^= 0x01;
    MINILSM_CHECK(
        DataBlockReader::Decode(bad_checksum, &reader).code() ==
        minilsm::Status::Code::kCorruption);
    return true;
}

}  // namespace

int main() {
    MINILSM_RUN(TestEmptyOneAndSeveralEntries);
    MINILSM_RUN(TestExactVersionOneBytes);
    MINILSM_RUN(TestExactTargetAndOversizedEntry);
    MINILSM_RUN(TestBuilderOrderingAndRecordValidation);
    MINILSM_RUN(TestMalformedTruncatedEntryAndBadChecksum);
    return 0;
}
