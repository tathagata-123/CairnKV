#include "iterator/merging_iterator.hpp"

#include <cassert>
#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "common/internal_key.hpp"

namespace minilsm {

bool MergingIterator::HeapComparator::operator()(
    const HeapEntry& left,
    const HeapEntry& right) const {
    assert(children != nullptr);
    const InternalKeyComparator compare;
    const int key_order = compare.Compare(
        (*children)[left.source_id]->key(),
        (*children)[right.source_id]->key());
    if (key_order != 0) {
        return key_order > 0;
    }
    return left.source_id > right.source_id;
}

MergingIterator::MergingIterator(
    std::vector<std::unique_ptr<InternalIterator>> children)
    : children_(std::move(children)),
      heap_(HeapComparator{&children_}) {}

void MergingIterator::ClearHeap() {
    heap_ = Heap(HeapComparator{&children_});
}

void MergingIterator::SetError(Status status) {
    status_ = std::move(status);
    ClearHeap();
}

Status MergingIterator::SeekToFirst() {
    return SeekChildren(nullptr);
}

Status MergingIterator::Seek(const InternalKey& target) {
    return SeekChildren(&target);
}

Status MergingIterator::SeekChildren(const InternalKey* target) {
    ClearHeap();
    status_ = Status::Ok();
    for (std::size_t source_id = 0U;
         source_id < children_.size();
         ++source_id) {
        const Status child_status = target == nullptr
            ? children_[source_id]->SeekToFirst()
            : children_[source_id]->Seek(*target);
        if (!child_status.ok()) {
            SetError(child_status);
            return status_;
        }
        if (!children_[source_id]->status().ok()) {
            SetError(children_[source_id]->status());
            return status_;
        }
        if (children_[source_id]->Valid()) {
            heap_.push(HeapEntry{source_id});
        }
    }
    return status_;
}

void MergingIterator::Next() {
    if (!Valid()) {
        return;
    }

    const HeapEntry current = heap_.top();
    heap_.pop();
    InternalIterator* child = children_[current.source_id].get();
    child->Next();
    if (!child->status().ok()) {
        SetError(child->status());
        return;
    }
    if (child->Valid()) {
        heap_.push(current);
    }
}

bool MergingIterator::Valid() const {
    return status_.ok() && !heap_.empty();
}

const InternalKey& MergingIterator::key() const {
    assert(Valid());
    return children_[heap_.top().source_id]->key();
}

std::string_view MergingIterator::value() const {
    assert(Valid());
    return children_[heap_.top().source_id]->value();
}

const Status& MergingIterator::status() const {
    return status_;
}

}  // namespace minilsm
