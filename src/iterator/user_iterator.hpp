#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "iterator/internal_iterator.hpp"
#include "minilsm/status.hpp"

namespace minilsm {

// Converts the merged InternalKey stream into at most one visible value for
// each user key. The merged stream is already newest-first within a key.
class UserIterator {
public:
    explicit UserIterator(
        std::unique_ptr<InternalIterator> internal_iterator);

    void Next();

    [[nodiscard]] bool Valid() const;
    [[nodiscard]] std::string_view key() const;
    [[nodiscard]] std::string_view value() const;
    [[nodiscard]] const Status& status() const;

private:
    void FindNextVisible();

    std::unique_ptr<InternalIterator> internal_iterator_;
    std::string key_;
    std::string value_;
    bool valid_ = false;
    Status status_ = Status::Ok();
};

}  // namespace minilsm
