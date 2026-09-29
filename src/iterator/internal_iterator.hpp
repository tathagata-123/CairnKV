#pragma once

#include <string_view>

#include "common/internal_key.hpp"
#include "minilsm/status.hpp"

namespace minilsm {

// The one small polymorphic interface used by the merge. Implementations
// are forward-only and expose complete InternalKey/value records.
class InternalIterator {
public:
    virtual ~InternalIterator() = default;

    virtual Status SeekToFirst() = 0;
    virtual Status Seek(const InternalKey& target) = 0;
    virtual void Next() = 0;

    [[nodiscard]] virtual bool Valid() const = 0;
    [[nodiscard]] virtual const InternalKey& key() const = 0;
    [[nodiscard]] virtual std::string_view value() const = 0;
    [[nodiscard]] virtual const Status& status() const = 0;
};

}  // namespace minilsm
