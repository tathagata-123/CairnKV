#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/file_io.hpp"
#include "common/internal_key.hpp"
#include "minilsm/status.hpp"
#include "sstable/block.hpp"
#include "sstable/bloom_filter.hpp"
#include "sstable/sstable_format.hpp"

namespace minilsm {

// Writes one immutable SSTable directly to a newly created file. The builder
// does not install the file into DB state; callers are responsible for paths.
class SstableBuilder {
public:
    static Status Create(
        const std::filesystem::path& path,
        std::size_t target_data_block_size,
        std::unique_ptr<SstableBuilder>* builder,
        bool prefix_compression = false);

    Status Add(const InternalKey& internal_key, std::string_view value);
    Status Finish();

private:
    SstableBuilder(
        std::unique_ptr<SequentialFileWriter> file_writer,
        std::size_t target_data_block_size, bool prefix_compression);

    Status FlushDataBlock();
    Status CheckAppendSize(std::size_t size) const;

    std::unique_ptr<SequentialFileWriter> file_writer_;
    std::size_t target_data_block_size_ = 0U;
    bool prefix_compression_ = false;
    DataBlockBuilder data_block_;
    std::vector<BlockIndexEntry> index_entries_;
    std::vector<std::string> bloom_user_keys_;
    std::optional<InternalKey> last_key_;
    std::uint64_t file_size_ = 0U;
    std::uint64_t entry_count_ = 0U;
    bool finished_ = false;
    bool failed_ = false;
};

}  // namespace minilsm
