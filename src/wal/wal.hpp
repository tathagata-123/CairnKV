#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/file_io.hpp"
#include "common/internal_key.hpp"
#include "minilsm/status.hpp"
#include "minilsm/write_batch.hpp"

namespace minilsm {

// Version 1 WAL layout from FILE_FORMATS.md:
//   32-byte file header
//   repeated [u32 payload_size][payload][u32 CRC-32C]
// Each payload is [u32 InternalKey size][InternalKey][u32 value size][value].
inline constexpr std::uint64_t kWalFileHeaderBytes = 32U;
inline constexpr std::uint32_t kMinWalPayloadBytes = 21U;
inline constexpr std::uint32_t kMaxWalPayloadBytes = 68'157'461U;
// An otherwise invalid legacy length introduces an atomic batch frame. Older
// engines reject this extension rather than replaying a prefix of a batch.
inline constexpr std::uint32_t kWalBatchMarker = 0xffffffffU;
inline constexpr std::uint32_t kMaxWalBatchBytes = 128U * 1024U * 1024U;
inline constexpr std::uint32_t kMaxWalBatchMutations = 1'000'000U;

struct WalRecord {
    InternalKey internal_key;
    std::string value;
};

class WalWriter {
public:
    static Status CreateNew(
        const std::filesystem::path& path,
        std::uint64_t wal_file_number,
        std::unique_ptr<WalWriter>* writer);
    static Status OpenForAppend(
        const std::filesystem::path& path,
        std::uint64_t file_size,
        std::uint64_t last_sequence,
        std::unique_ptr<WalWriter>* writer);

    Status AppendAndSync(
        const InternalKey& internal_key,
        std::string_view value);
    Status AppendBatch(std::uint64_t start_sequence, const WriteBatch& batch, bool sync);
    Status Sync();
    Status TruncateTo(std::uint64_t valid_size);
    Status Close();

    // Narrow deterministic hooks for the durability-ordering tests.
    void FailNextAppendForTesting();
    void FailNextSyncForTesting();

private:
    WalWriter(
        std::unique_ptr<SequentialFileWriter> file_writer,
        std::uint64_t file_size,
        std::uint64_t last_sequence);

    Status AppendFrame(std::string_view frame, std::uint64_t last_sequence, bool sync);

    std::unique_ptr<SequentialFileWriter> file_writer_;
    std::uint64_t file_size_ = 0U;
    std::uint64_t last_sequence_ = 0U;
    bool failed_ = false;
    bool fail_next_append_for_testing_ = false;
    bool fail_next_sync_for_testing_ = false;
};

class WalReader {
public:
    static Status Open(
        const std::filesystem::path& path,
        std::uint64_t expected_wal_file_number,
        std::unique_ptr<WalReader>* reader);

    Status ReadNext(WalRecord* record, bool* at_end);
    Status Close();

    [[nodiscard]] std::uint64_t file_size() const;
    [[nodiscard]] std::uint64_t valid_size() const;
    [[nodiscard]] bool has_torn_tail() const;

private:
    WalReader(
        std::unique_ptr<RandomAccessFileReader> file_reader,
        std::uint64_t file_size);

    std::unique_ptr<RandomAccessFileReader> file_reader_;
    std::uint64_t file_size_ = 0U;
    std::uint64_t offset_ = kWalFileHeaderBytes;
    std::uint64_t last_sequence_ = 0U;
    std::vector<WalRecord> pending_records_;
    std::size_t pending_index_ = 0U;
    bool at_end_ = false;
    bool has_torn_tail_ = false;
};

}  // namespace minilsm
