#include "minilsm/status.hpp"

#include <utility>

namespace minilsm {

Status::Status(Code code, std::string message)
    : code_(code), message_(std::move(message)) {}

Status Status::Ok() {
    return Status();
}

Status Status::NotFound(std::string message) {
    return Status(Code::kNotFound, std::move(message));
}

Status Status::InvalidArgument(std::string message) {
    return Status(Code::kInvalidArgument, std::move(message));
}

Status Status::IOError(std::string message) {
    return Status(Code::kIOError, std::move(message));
}

Status Status::Corruption(std::string message) {
    return Status(Code::kCorruption, std::move(message));
}

Status Status::NotSupported(std::string message) {
    return Status(Code::kNotSupported, std::move(message));
}

bool Status::ok() const {
    return code_ == Code::kOk;
}

Status::Code Status::code() const {
    return code_;
}

const std::string& Status::message() const {
    return message_;
}

}  // namespace minilsm
