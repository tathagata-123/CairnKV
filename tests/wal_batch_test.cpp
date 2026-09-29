#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include "common/checksum.hpp"
#include "common/coding.hpp"
#include "common/file_io.hpp"
#include "test_harness.hpp"
#include "wal/wal_format.hpp"

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string name = (std::filesystem::temp_directory_path() / "minilsm-batch-XXXXXX").string();
        std::vector<char> bytes(name.begin(), name.end());
        bytes.push_back('\0');
        if (const char* made = ::mkdtemp(bytes.data())) path = made;
    }
    ~TemporaryDirectory() {
        std::error_code error;
        if (!path.empty()) std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};

minilsm::WriteBatch ExampleBatch() {
    minilsm::WriteBatch batch;
    batch.Put("a", "old");
    batch.Put("b", "value");
    batch.Delete("a");
    batch.Put("a", "new");
    return batch;
}

bool WriteFile(const std::filesystem::path& path, std::string_view bytes) {
    std::unique_ptr<minilsm::SequentialFileWriter> writer;
    MINILSM_CHECK(minilsm::SequentialFileWriter::CreateNew(path, &writer).ok());
    MINILSM_CHECK(writer->Append(bytes).ok());
    MINILSM_CHECK(writer->Close().ok());
    return true;
}

void Store32(std::string* bytes, std::size_t offset, std::uint32_t value) {
    std::string encoded;
    minilsm::PutFixed32(&encoded, value);
    bytes->replace(offset, 4U, encoded);
}

void RefreshFrameChecksum(std::string* bytes) {
    Store32(bytes, bytes->size() - 4U,
            minilsm::Crc32c(std::string_view(*bytes).substr(0U, bytes->size() - 4U)));
}

bool TestMixedLegacyAndBatchRoundTrip() {
    TemporaryDirectory dir;
    MINILSM_CHECK(!dir.path.empty());
    const auto path = dir.path / "wal";
    std::unique_ptr<minilsm::WalWriter> writer;
    MINILSM_CHECK(minilsm::WalWriter::CreateNew(path, 7U, &writer).ok());
    MINILSM_CHECK(writer->AppendAndSync({"legacy", 1U, minilsm::RecordType::kValue}, "old").ok());
    const auto batch = ExampleBatch();
    MINILSM_CHECK(writer->AppendBatch(2U, batch, false).ok());
    MINILSM_CHECK(writer->AppendBatch(5U, batch, true).code() == minilsm::Status::Code::kInvalidArgument);
    MINILSM_CHECK(writer->AppendBatch(6U, {}, true).ok());
    MINILSM_CHECK(writer->Sync().ok());
    MINILSM_CHECK(writer->AppendAndSync({"last", 6U, minilsm::RecordType::kValue}, "done").ok());
    MINILSM_CHECK(writer->Close().ok());
    std::unique_ptr<minilsm::WalReader> reader;
    MINILSM_CHECK(minilsm::WalReader::Open(path, 7U, &reader).ok());
    bool at_end = false;
    minilsm::WalRecord record;
    for (std::uint64_t sequence = 1U; sequence <= 6U; ++sequence) {
        MINILSM_CHECK(reader->ReadNext(&record, &at_end).ok() && !at_end);
        MINILSM_CHECK(record.internal_key.sequence_number == sequence);
        if (sequence == 4U) MINILSM_CHECK(record.internal_key.record_type == minilsm::RecordType::kTombstone);
        if (sequence == 5U) MINILSM_CHECK(record.value == "new");
    }
    MINILSM_CHECK(reader->ReadNext(&record, &at_end).ok() && at_end);
    return true;
}

bool TestEveryTornBatchBoundaryPublishesNone() {
    std::string header, prefix, frame;
    MINILSM_CHECK(minilsm::EncodeWalHeader(1U, &header).ok());
    MINILSM_CHECK(minilsm::EncodeWalFrame({"prefix", 1U, minilsm::RecordType::kValue}, "safe", &prefix).ok());
    MINILSM_CHECK(minilsm::EncodeWalBatchFrame(2U, ExampleBatch(), &frame).ok());
    TemporaryDirectory dir;
    for (std::size_t cut = 1U; cut < frame.size(); ++cut) {
        const auto path = dir.path / std::to_string(cut);
        MINILSM_CHECK(WriteFile(path, header + prefix + frame.substr(0U, cut)));
        std::unique_ptr<minilsm::WalReader> reader;
        MINILSM_CHECK(minilsm::WalReader::Open(path, 1U, &reader).ok());
        minilsm::WalRecord record;
        bool at_end = false;
        MINILSM_CHECK(reader->ReadNext(&record, &at_end).ok() && !at_end);
        MINILSM_CHECK(record.internal_key.user_key == "prefix");
        MINILSM_CHECK(reader->ReadNext(&record, &at_end).ok() && at_end);
        MINILSM_CHECK(reader->has_torn_tail());
        MINILSM_CHECK(reader->valid_size() == header.size() + prefix.size());
    }
    return true;
}

bool TestCorruptionAndParserAtomicity() {
    std::string header, original;
    MINILSM_CHECK(minilsm::EncodeWalHeader(1U, &header).ok());
    MINILSM_CHECK(minilsm::EncodeWalBatchFrame(1U, ExampleBatch(), &original).ok());
    TemporaryDirectory dir;
    for (std::size_t kind = 0U; kind < 5U; ++kind) {
        std::string frame = original;
        if (kind == 0U) frame[4U] ^= 1; // length corruption must not become a torn tail
        if (kind == 1U) frame[frame.size() - 5U] ^= 1;
        if (kind == 2U) { // valid CRC but impossible mutation count
            Store32(&frame, 20U, 0xffffffffU);
            RefreshFrameChecksum(&frame);
        }
        if (kind == 3U) { // invalid final mutation: earlier ones must not leak
            const std::size_t last_mutation = frame.size() - 4U - 9U - 1U - 3U;
            frame[last_mutation] = 9;
            RefreshFrameChecksum(&frame);
        }
        if (kind == 4U) { // malformed count with trailing mutations
            Store32(&frame, 20U, 1U);
            RefreshFrameChecksum(&frame);
        }
        std::vector<minilsm::WalRecord> unchanged = {{{"sentinel", 99U, minilsm::RecordType::kValue}, "x"}};
        MINILSM_CHECK(minilsm::DecodeWalBatchFrame(frame, &unchanged).code() == minilsm::Status::Code::kCorruption);
        MINILSM_CHECK(unchanged.size() == 1U && unchanged.front().value == "x");
        const auto path = dir.path / std::to_string(kind);
        MINILSM_CHECK(WriteFile(path, header + frame));
        std::unique_ptr<minilsm::WalReader> reader;
        MINILSM_CHECK(minilsm::WalReader::Open(path, 1U, &reader).ok());
        minilsm::WalRecord record;
        bool at_end = false;
        MINILSM_CHECK(reader->ReadNext(&record, &at_end).code() == minilsm::Status::Code::kCorruption);
        MINILSM_CHECK(reader->valid_size() == header.size());
    }
    return true;
}

bool TestOversizedEnvelopeRejectedBeforePayloadRead() {
    TemporaryDirectory dir;
    std::string header, frame;
    MINILSM_CHECK(minilsm::EncodeWalHeader(1U, &header).ok());
    minilsm::PutFixed32(&frame, minilsm::kWalBatchMarker);
    minilsm::PutFixed32(&frame, minilsm::kMaxWalBatchBytes + 1U);
    minilsm::PutFixed32(&frame, minilsm::Crc32c(frame));
    // Only 12 frame bytes exist: no giant input or giant allocation is needed
    // to verify that an oversized but checksummed envelope is rejected.
    const auto path = dir.path / "oversized";
    MINILSM_CHECK(WriteFile(path, header + frame));
    std::unique_ptr<minilsm::WalReader> reader;
    MINILSM_CHECK(minilsm::WalReader::Open(path, 1U, &reader).ok());
    minilsm::WalRecord record;
    bool at_end = false;
    MINILSM_CHECK(reader->ReadNext(&record, &at_end).code() == minilsm::Status::Code::kCorruption);
    MINILSM_CHECK(!reader->has_torn_tail());
    MINILSM_CHECK(reader->valid_size() == header.size());
    return true;
}

bool TestOverflowAndStickySyncError() {
    std::string encoded;
    MINILSM_CHECK(minilsm::EncodeWalBatchFrame(std::numeric_limits<std::uint64_t>::max(), ExampleBatch(), &encoded).code() == minilsm::Status::Code::kInvalidArgument);
    MINILSM_CHECK(minilsm::EncodeWalBatchFrame(0U, ExampleBatch(), &encoded).code() == minilsm::Status::Code::kInvalidArgument);
    minilsm::WriteBatch singleton;
    singleton.Put("a", "1");
    MINILSM_CHECK(minilsm::EncodeWalBatchFrame(std::numeric_limits<std::uint64_t>::max(), singleton, &encoded).ok());
    std::vector<minilsm::WalRecord> decoded;
    MINILSM_CHECK(minilsm::DecodeWalBatchFrame(encoded, &decoded).ok());
    MINILSM_CHECK(decoded.front().internal_key.sequence_number == std::numeric_limits<std::uint64_t>::max());
    TemporaryDirectory dir;
    std::unique_ptr<minilsm::WalWriter> writer;
    MINILSM_CHECK(minilsm::WalWriter::CreateNew(dir.path / "wal", 1U, &writer).ok());
    writer->FailNextSyncForTesting();
    MINILSM_CHECK(writer->AppendBatch(1U, singleton, false).ok());
    MINILSM_CHECK(writer->Sync().code() == minilsm::Status::Code::kIOError);
    MINILSM_CHECK(writer->AppendBatch(2U, singleton, true).code() == minilsm::Status::Code::kIOError);
    return true;
}

}  // namespace

int main() {
    MINILSM_RUN(TestMixedLegacyAndBatchRoundTrip);
    MINILSM_RUN(TestEveryTornBatchBoundaryPublishesNone);
    MINILSM_RUN(TestCorruptionAndParserAtomicity);
    MINILSM_RUN(TestOversizedEnvelopeRejectedBeforePayloadRead);
    MINILSM_RUN(TestOverflowAndStickySyncError);
    return 0;
}
