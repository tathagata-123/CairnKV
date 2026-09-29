#include "sstable/sstable_iterator.hpp"

#include <cassert>
#include <cstddef>
#include <string>
#include <string_view>

namespace minilsm {

SstableIterator::SstableIterator(const SstableReader* reader)
    : reader_(reader) {}

Status SstableIterator::LoadBlock(
    std::size_t block_index,
    std::size_t entry_index) {
    std::shared_ptr<const DataBlockReader> decoded;
    Status status = reader_->ReadSharedDataBlock(block_index, &decoded);
    if (!status.ok()) {
        status_ = status;
        valid_ = false;
        return status_;
    }
    if (entry_index >= decoded->entries().size()) {
        status_ = Status::Corruption(
            "SSTable iterator entry index is outside its block");
        valid_ = false;
        return status_;
    }

    block_ = std::move(decoded);
    block_index_ = block_index;
    entry_index_ = entry_index;
    valid_ = true;
    status_ = Status::Ok();
    return status_;
}

void SstableIterator::SetEnd() {
    valid_ = false;
    status_ = Status::Ok();
}

Status SstableIterator::SeekToFirst() {
    if (reader_ == nullptr) {
        status_ = Status::InvalidArgument(
            "SSTable iterator has no reader");
        valid_ = false;
        return status_;
    }
    if (reader_->index().empty()) {
        SetEnd();
        return status_;
    }
    return LoadBlock(0U, 0U);
}

Status SstableIterator::Seek(const InternalKey& target) {
    if (reader_ == nullptr) {
        status_ = Status::InvalidArgument(
            "SSTable iterator has no reader");
        valid_ = false;
        return status_;
    }
    std::string encoded_target;
    Status status = EncodeInternalKey(target, &encoded_target);
    if (!status.ok()) {
        status_ = status;
        valid_ = false;
        return status_;
    }

    const std::size_t block_index =
        reader_->FindCandidateBlock(target);
    if (block_index == reader_->index().size()) {
        SetEnd();
        return status_;
    }

    std::shared_ptr<const DataBlockReader> decoded;
    status = reader_->ReadSharedDataBlock(block_index, &decoded);
    if (!status.ok()) {
        status_ = status;
        valid_ = false;
        return status_;
    }
    const std::size_t entry_index = decoded->LowerBound(target);
    if (entry_index == decoded->entries().size()) {
        status_ = Status::Corruption(
            "sparse index candidate does not contain its boundary");
        valid_ = false;
        return status_;
    }

    block_ = std::move(decoded);
    block_index_ = block_index;
    entry_index_ = entry_index;
    valid_ = true;
    status_ = Status::Ok();
    return status_;
}

void SstableIterator::Next() {
    if (!valid_) {
        return;
    }
    ++entry_index_;
    if (entry_index_ < block_->entries().size()) {
        return;
    }

    const std::size_t next_block = block_index_ + 1U;
    if (next_block == reader_->index().size()) {
        SetEnd();
        return;
    }
    LoadBlock(next_block, 0U);
}

bool SstableIterator::Valid() const {
    return valid_;
}

const InternalKey& SstableIterator::key() const {
    assert(valid_);
    return block_->entries()[entry_index_].internal_key;
}

std::string_view SstableIterator::value() const {
    assert(valid_);
    return block_->entries()[entry_index_].value;
}

const Status& SstableIterator::status() const {
    return status_;
}

}  // namespace minilsm
