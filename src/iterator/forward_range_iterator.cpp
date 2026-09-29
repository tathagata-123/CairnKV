#include "iterator/forward_range_iterator.hpp"

#include <cassert>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "common/internal_key.hpp"

namespace minilsm {

ForwardRangeIterator::ForwardRangeIterator(
    std::unique_ptr<UserIterator> user_iterator,
    std::optional<std::string> upper_bound)
    : user_iterator_(std::move(user_iterator)),
      upper_bound_(std::move(upper_bound)) {
    ApplyUpperBound();
}

ForwardRangeIterator::ForwardRangeIterator(Status status)
    : status_(std::move(status)) {}

void ForwardRangeIterator::ApplyUpperBound() {
    if (user_iterator_ != nullptr && user_iterator_->Valid() &&
        upper_bound_.has_value() &&
        CompareUserKeys(user_iterator_->key(), *upper_bound_) >= 0) {
        reached_upper_bound_ = true;
    }
}

bool ForwardRangeIterator::Valid() const {
    return status_.ok() && !reached_upper_bound_ &&
           user_iterator_ != nullptr && user_iterator_->Valid();
}

std::string_view ForwardRangeIterator::key() const {
    assert(Valid());
    return user_iterator_->key();
}

std::string_view ForwardRangeIterator::value() const {
    assert(Valid());
    return user_iterator_->value();
}

void ForwardRangeIterator::Next() {
    if (!Valid()) {
        return;
    }
    user_iterator_->Next();
    ApplyUpperBound();
}

const Status& ForwardRangeIterator::status() const {
    if (!status_.ok() || user_iterator_ == nullptr) {
        return status_;
    }
    return user_iterator_->status();
}

}  // namespace minilsm
