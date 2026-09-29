#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "common/file_io.hpp"
#include "common/internal_key.hpp"
#include "minilsm/status.hpp"
#include "sstable/block.hpp"
#include "sstable/bloom_filter.hpp"
#include "sstable/sstable_format.hpp"

namespace minilsm {

class BlockCache;

struct SstableProperties {
    std::uint64_t file_size = 0U;
    std::uint64_t entry_count = 0U;
    std::uint64_t smallest_sequence = 0U;
    std::uint64_t largest_sequence = 0U;
    InternalKey smallest_internal_key;
    InternalKey largest_internal_key;
};

class SstableReader {
public:
    static Status Open(
        const std::filesystem::path& path,
        std::unique_ptr<SstableReader>* reader);

    void SetBlockCache(std::shared_ptr<BlockCache> cache, std::uint64_t file_number);
    Status ReadSharedDataBlock(std::size_t block_index,
        std::shared_ptr<const DataBlockReader>* block) const;

    [[nodiscard]] LookupResult Get(const InternalKey& target) const;
    [[nodiscard]] bool MayContain(std::string_view user_key) const;
    [[nodiscard]] bool has_bloom_filter() const;
    [[nodiscard]] std::uint64_t bloom_filter_key_count() const;

    [[nodiscard]] std::size_t FindCandidateBlock(
        const InternalKey& target) const;
    Status ReadDataBlock(
        std::size_t block_index,
        DataBlockReader* block) const;
    Status ValidateAndReadProperties(SstableProperties* properties) const;

    [[nodiscard]] const std::vector<BlockIndexEntry>& index() const;
    [[nodiscard]] const SstableFooter& footer() const;
    [[nodiscard]] std::uint64_t entry_count() const;
    [[nodiscard]] std::uint64_t file_size() const;

private:
    SstableReader(
        std::unique_ptr<RandomAccessFileReader> file_reader,
        std::uint64_t file_size,
        SstableFooter footer,
        std::vector<BlockIndexEntry> index_entries,
        std::optional<BloomFilter> bloom_filter);

    std::shared_ptr<BlockCache> block_cache_;
    std::uint64_t file_number_ = 0;
    std::unique_ptr<RandomAccessFileReader> file_reader_;
    std::uint64_t file_size_ = 0U;
    SstableFooter footer_;
    std::vector<BlockIndexEntry> index_entries_;
    std::optional<BloomFilter> bloom_filter_;
};

}  // namespace minilsm
