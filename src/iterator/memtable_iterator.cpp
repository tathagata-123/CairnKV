#include "iterator/memtable_iterator.hpp"

#include <cassert>
#include <string_view>

namespace minilsm {

MemTableIterator::MemTableIterator(
    const MemTable::RecordMap* records)
    : owned_records_(records == nullptr ? MemTable::RecordMap{} : *records),
      records_(records == nullptr ? nullptr : &owned_records_) {}

Status MemTableIterator::SeekToFirst() {
    if (records_ == nullptr) {
        status_ = Status::InvalidArgument(
            "memtable iterator has no record map");
        valid_ = false;
        return status_;
    }
    current_ = records_->begin();
    valid_ = current_ != records_->end();
    status_ = Status::Ok();
    return status_;
}

Status MemTableIterator::Seek(const InternalKey& target) {
    if (records_ == nullptr) {
        status_ = Status::InvalidArgument(
            "memtable iterator has no record map");
        valid_ = false;
        return status_;
    }
    current_ = records_->lower_bound(target);
    valid_ = current_ != records_->end();
    status_ = Status::Ok();
    return status_;
}

void MemTableIterator::Next() {
    if (!valid_) {
        return;
    }
    ++current_;
    valid_ = current_ != records_->end();
}

bool MemTableIterator::Valid() const {
    return valid_;
}

const InternalKey& MemTableIterator::key() const {
    assert(valid_);
    return current_->first;
}

std::string_view MemTableIterator::value() const {
    assert(valid_);
    return current_->second;
}

const Status& MemTableIterator::status() const {
    return status_;
}

}  // namespace minilsm
