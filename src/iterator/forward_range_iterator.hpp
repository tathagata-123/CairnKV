#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "iterator/user_iterator.hpp"
#include "minilsm/iterator.hpp"
#include "minilsm/status.hpp"

namespace minilsm {

// The public API layer. Lower-bound positioning has already happened in the
// internal merge; this wrapper stops before the exclusive upper bound.
class ForwardRangeIterator final : public Iterator {
public:
    ForwardRangeIterator(
        std::unique_ptr<UserIterator> user_iterator,
        std::optional<std::string> upper_bound);
    explicit ForwardRangeIterator(Status status);

    [[nodiscard]] bool Valid() const override;
    [[nodiscard]] std::string_view key() const override;
    [[nodiscard]] std::string_view value() const override;
    void Next() override;
    [[nodiscard]] const Status& status() const override;

private:
    void ApplyUpperBound();

    std::unique_ptr<UserIterator> user_iterator_;
    std::optional<std::string> upper_bound_;
    Status status_ = Status::Ok();
    bool reached_upper_bound_ = false;
};

}  // namespace minilsm
