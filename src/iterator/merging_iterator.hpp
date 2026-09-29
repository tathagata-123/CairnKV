#pragma once

#include <cstddef>
#include <memory>
#include <queue>
#include <vector>

#include "iterator/internal_iterator.hpp"

namespace minilsm {

// A forward k-way min-heap merge. Heap entries identify child iterators;
// child state is never copied. Exact InternalKey ties use source_id only to
// make duplicate processing deterministic.
class MergingIterator final : public InternalIterator {
public:
    explicit MergingIterator(
        std::vector<std::unique_ptr<InternalIterator>> children);

    MergingIterator(const MergingIterator&) = delete;
    MergingIterator& operator=(const MergingIterator&) = delete;
    MergingIterator(MergingIterator&&) = delete;
    MergingIterator& operator=(MergingIterator&&) = delete;

    Status SeekToFirst() override;
    Status Seek(const InternalKey& target) override;
    void Next() override;

    [[nodiscard]] bool Valid() const override;
    [[nodiscard]] const InternalKey& key() const override;
    [[nodiscard]] std::string_view value() const override;
    [[nodiscard]] const Status& status() const override;

private:
    struct HeapEntry {
        std::size_t source_id = 0U;
    };

    struct HeapComparator {
        const std::vector<std::unique_ptr<InternalIterator>>* children =
            nullptr;

        [[nodiscard]] bool operator()(
            const HeapEntry& left,
            const HeapEntry& right) const;
    };

    using Heap = std::priority_queue<
        HeapEntry,
        std::vector<HeapEntry>,
        HeapComparator>;

    void ClearHeap();
    void SetError(Status status);
    Status SeekChildren(const InternalKey* target);

    std::vector<std::unique_ptr<InternalIterator>> children_;
    Heap heap_;
    Status status_ = Status::Ok();
};

}  // namespace minilsm
