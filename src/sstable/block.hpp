#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/internal_key.hpp"
#include "minilsm/status.hpp"

namespace minilsm {

struct DataBlockEntry {
    InternalKey internal_key;
    std::string value;
};

// Builds legacy blocks or prefix-compressed blocks with 16-entry restarts. The builder does not choose block
// boundaries; SstableBuilder applies the configured physical-size target.
class DataBlockBuilder {
public:
    explicit DataBlockBuilder(bool prefix_compression = false)
        : prefix_compression_(prefix_compression) {}
    Status Add(const InternalKey& internal_key, std::string_view value);
    Status Finish(std::string* block) const;

    [[nodiscard]] bool empty() const;
    [[nodiscard]] std::uint32_t entry_count() const;
    [[nodiscard]] std::size_t current_physical_size() const;
    [[nodiscard]] std::size_t uncompressed_entry_bytes() const { return uncompressed_bytes_; }
    [[nodiscard]] const InternalKey& last_key() const;

private:
    bool prefix_compression_ = false;
    std::vector<std::uint32_t> restart_offsets_;
    std::string entries_bytes_;
    std::size_t uncompressed_bytes_ = 0;
    std::uint32_t entry_count_ = 0U;
    std::optional<InternalKey> last_key_;
};

// Validates and owns the decoded entries from one complete framed data block.
class DataBlockReader {
public:
    static Status Decode(
        std::string_view physical_block,
        DataBlockReader* reader);

    [[nodiscard]] const std::vector<DataBlockEntry>& entries() const;
    [[nodiscard]] std::size_t MemoryUsage() const;
    [[nodiscard]] std::size_t LowerBound(
        const InternalKey& target) const;

private:
    std::vector<DataBlockEntry> entries_;
    std::vector<std::size_t> restart_entries_;
};

}  // namespace minilsm
