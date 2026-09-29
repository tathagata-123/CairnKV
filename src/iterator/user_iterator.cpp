#include "iterator/user_iterator.hpp"

#include <cassert>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "common/internal_key.hpp"

namespace minilsm {

UserIterator::UserIterator(
    std::unique_ptr<InternalIterator> internal_iterator)
    : internal_iterator_(std::move(internal_iterator)) {
    FindNextVisible();
}

void UserIterator::FindNextVisible() {
    valid_ = false;
    key_.clear();
    value_.clear();

    while (internal_iterator_->Valid()) {
        const std::string user_key =
            internal_iterator_->key().user_key;
        const RecordType newest_type =
            internal_iterator_->key().record_type;
        std::string newest_value;
        if (newest_type == RecordType::kValue) {
            newest_value.assign(internal_iterator_->value());
        }

        // Consume the chosen record, every exact duplicate, and every older
        // version before making this user key visible to the public layer.
        do {
            internal_iterator_->Next();
            if (!internal_iterator_->status().ok()) {
                status_ = internal_iterator_->status();
                return;
            }
        } while (internal_iterator_->Valid() &&
                 internal_iterator_->key().user_key == user_key);

        if (newest_type == RecordType::kTombstone) {
            continue;
        }
        key_ = user_key;
        value_ = std::move(newest_value);
        valid_ = true;
        status_ = Status::Ok();
        return;
    }

    status_ = internal_iterator_->status();
}

void UserIterator::Next() {
    if (!valid_) {
        return;
    }
    FindNextVisible();
}

bool UserIterator::Valid() const {
    return valid_ && status_.ok();
}

std::string_view UserIterator::key() const {
    assert(Valid());
    return key_;
}

std::string_view UserIterator::value() const {
    assert(Valid());
    return value_;
}

const Status& UserIterator::status() const {
    return status_;
}

}  // namespace minilsm
