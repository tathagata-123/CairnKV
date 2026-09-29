#pragma once

#include <memory>

#include "iterator/internal_iterator.hpp"
#include "sstable/sstable_iterator.hpp"
#include "sstable/sstable_reader.hpp"

namespace minilsm {

// Owns the reader required by the SSTable iterator. Member order keeps
// the reader alive until after the adapted iterator is destroyed.
class SstableIteratorAdapter final : public InternalIterator {
public:
    explicit SstableIteratorAdapter(
        std::shared_ptr<SstableReader> reader);

    Status SeekToFirst() override;
    Status Seek(const InternalKey& target) override;
    void Next() override;

    [[nodiscard]] bool Valid() const override;
    [[nodiscard]] const InternalKey& key() const override;
    [[nodiscard]] std::string_view value() const override;
    [[nodiscard]] const Status& status() const override;

private:
    std::shared_ptr<SstableReader> reader_;
    SstableIterator iterator_;
};

}  // namespace minilsm
