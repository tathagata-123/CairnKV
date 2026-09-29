#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace minilsm {

enum class MutationType : std::uint8_t { kDelete = 0, kPut = 1 };

struct Mutation {
    MutationType type;
    std::string key;
    std::string value;
};

struct WriteOptions {
    // false permits loss after a machine crash; each batch remains atomic.
    bool sync = true;
};

// Owns its strings. Mutation order determines the order of assigned sequences.
class WriteBatch {
public:
    void Put(std::string_view key, std::string_view value) {
        mutations_.push_back({MutationType::kPut, std::string(key), std::string(value)});
    }
    void Delete(std::string_view key) {
        mutations_.push_back({MutationType::kDelete, std::string(key), {}});
    }
    void Clear() { mutations_.clear(); }
    [[nodiscard]] bool empty() const { return mutations_.empty(); }
    [[nodiscard]] std::size_t size() const { return mutations_.size(); }
    [[nodiscard]] const std::vector<Mutation>& mutations() const { return mutations_; }

private:
    std::vector<Mutation> mutations_;
};

}  // namespace minilsm
