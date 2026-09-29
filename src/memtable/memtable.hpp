#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>

#include "common/internal_key.hpp"
#include "minilsm/status.hpp"

namespace minilsm {

enum class MemTableLookupState {
    kNotPresent,
    kValue,
    kTombstone,
};

struct MemTableLookup {
    MemTableLookupState state = MemTableLookupState::kNotPresent;
    std::string value;
};

class MemTable {
public:
    using RecordMap =
        std::map<InternalKey, std::string, InternalKeyComparator>;

    Status Add(InternalKey internal_key, std::string value);
    [[nodiscard]] MemTableLookup Lookup(
        std::string_view user_key) const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool empty() const;
    [[nodiscard]] std::size_t approximate_memory_usage() const;
    [[nodiscard]] const RecordMap& records() const;
    void Clear();

private:
    // InternalKeyComparator makes each user key's newest sequence the first
    // entry in its group. Older versions and tombstones remain stored.
    RecordMap records_;
    std::size_t approximate_memory_usage_ = 0U;
};

}  // namespace minilsm
