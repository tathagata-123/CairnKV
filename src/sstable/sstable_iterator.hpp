#pragma once

#include <cstddef>
#include <string_view>

#include "common/internal_key.hpp"
#include "minilsm/status.hpp"
#include "sstable/block.hpp"
#include "sstable/sstable_reader.hpp"

namespace minilsm {

// Forward iterator over one SSTable. The referenced reader must outlive the
// iterator. It performs no user-key deduplication or tombstone suppression.
class SstableIterator {
public:
    explicit SstableIterator(const SstableReader* reader);

    Status SeekToFirst();
    Status Seek(const InternalKey& target);
    void Next();

    [[nodiscard]] bool Valid() const;
    [[nodiscard]] const InternalKey& key() const;
    [[nodiscard]] std::string_view value() const;
    [[nodiscard]] const Status& status() const;

private:
    Status LoadBlock(std::size_t block_index, std::size_t entry_index);
    void SetEnd();

    const SstableReader* reader_ = nullptr;
    std::shared_ptr<const DataBlockReader> block_;
    std::size_t block_index_ = 0U;
    std::size_t entry_index_ = 0U;
    bool valid_ = false;
    Status status_ = Status::Ok();
};

}  // namespace minilsm
