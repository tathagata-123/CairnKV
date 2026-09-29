#pragma once

#include <optional>
#include <string>

namespace minilsm {

class Status {
public:
    enum class Code {
        kOk = 0,
        kNotFound,
        kInvalidArgument,
        kIOError,
        kCorruption,
        kNotSupported,
    };

    Status() = default;

    static Status Ok();
    static Status NotFound(std::string message);
    static Status InvalidArgument(std::string message);
    static Status IOError(std::string message);
    static Status Corruption(std::string message);
    static Status NotSupported(std::string message);

    [[nodiscard]] bool ok() const;
    [[nodiscard]] Code code() const;
    [[nodiscard]] const std::string& message() const;

private:
    Status(Code code, std::string message);

    Code code_ = Code::kOk;
    std::string message_;
};

struct LookupResult {
    Status status;
    std::optional<std::string> value;
};

}  // namespace minilsm
