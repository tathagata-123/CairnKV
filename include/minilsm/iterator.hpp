#pragma once

#include <string_view>

#include "minilsm/status.hpp"

namespace minilsm {

class Iterator {
public:
    virtual ~Iterator() = default;

    // key() and value() are valid only while Valid() is true. Once Valid()
    // becomes false, status() distinguishes normal exhaustion from an error.
    [[nodiscard]] virtual bool Valid() const = 0;
    [[nodiscard]] virtual std::string_view key() const = 0;
    [[nodiscard]] virtual std::string_view value() const = 0;
    virtual void Next() = 0;
    [[nodiscard]] virtual const Status& status() const = 0;
};

}  // namespace minilsm
