#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/internal_key.hpp"
#include "iterator/internal_iterator.hpp"
#include "iterator/memtable_iterator.hpp"
#include "iterator/merging_iterator.hpp"
#include "iterator/user_iterator.hpp"
#include "memtable/memtable.hpp"
#include "minilsm/status.hpp"
#include "test_harness.hpp"

namespace {

struct Entry {
    minilsm::InternalKey key;
    std::string value;
};

using UserEntry = std::pair<std::string, std::string>;

Entry Value(
    std::string user_key,
    std::uint64_t sequence,
    std::string value) {
    return Entry{
        minilsm::InternalKey{
            std::move(user_key),
            sequence,
            minilsm::RecordType::kValue},
        std::move(value),
    };
}

Entry Tombstone(std::string user_key, std::uint64_t sequence) {
    return Entry{
        minilsm::InternalKey{
            std::move(user_key),
            sequence,
            minilsm::RecordType::kTombstone},
        {},
    };
}

bool KeysEqual(
    const minilsm::InternalKey& left,
    const minilsm::InternalKey& right) {
    return left.user_key == right.user_key &&
           left.sequence_number == right.sequence_number &&
           left.record_type == right.record_type;
}

class VectorInternalIterator final : public minilsm::InternalIterator {
public:
    explicit VectorInternalIterator(
        std::vector<Entry> entries,
        std::size_t fail_on_advance =
            std::numeric_limits<std::size_t>::max())
        : entries_(std::move(entries)),
          fail_on_advance_(fail_on_advance) {}

    minilsm::Status SeekToFirst() override {
        position_ = 0U;
        advances_ = 0U;
        status_ = minilsm::Status::Ok();
        return status_;
    }

    minilsm::Status Seek(const minilsm::InternalKey& target) override {
        const minilsm::InternalKeyComparator compare;
        const auto found = std::lower_bound(
            entries_.begin(),
            entries_.end(),
            target,
            [&compare](const Entry& entry,
                       const minilsm::InternalKey& key) {
                return compare(entry.key, key);
            });
        position_ = static_cast<std::size_t>(found - entries_.begin());
        advances_ = 0U;
        status_ = minilsm::Status::Ok();
        return status_;
    }

    void Next() override {
        if (!Valid()) {
            return;
        }
        ++advances_;
        if (advances_ == fail_on_advance_) {
            status_ = minilsm::Status::Corruption(
                "simulated child corruption");
            position_ = entries_.size();
            return;
        }
        ++position_;
    }

    [[nodiscard]] bool Valid() const override {
        return status_.ok() && position_ < entries_.size();
    }

    [[nodiscard]] const minilsm::InternalKey& key() const override {
        assert(Valid());
        return entries_[position_].key;
    }

    [[nodiscard]] std::string_view value() const override {
        assert(Valid());
        return entries_[position_].value;
    }

    [[nodiscard]] const minilsm::Status& status() const override {
        return status_;
    }

private:
    std::vector<Entry> entries_;
    std::size_t position_ = 0U;
    std::size_t advances_ = 0U;
    std::size_t fail_on_advance_ =
        std::numeric_limits<std::size_t>::max();
    minilsm::Status status_ = minilsm::Status::Ok();
};

std::unique_ptr<minilsm::InternalIterator> Child(
    std::vector<Entry> entries) {
    return std::make_unique<VectorInternalIterator>(std::move(entries));
}

bool TestMemTableIterationAndSeek() {
    minilsm::MemTable memtable;
    MINILSM_CHECK(memtable.Add(
        Value("b", 1U, "b-old").key, "b-old").ok());
    MINILSM_CHECK(memtable.Add(
        Value("a", 4U, "a").key, "a").ok());
    MINILSM_CHECK(memtable.Add(
        Value(
            "b",
            std::numeric_limits<std::uint64_t>::max(),
            "b-new")
            .key,
        "b-new").ok());
    MINILSM_CHECK(memtable.Add(
        Tombstone("c", 3U).key, "").ok());

    minilsm::MemTableIterator iterator(&memtable.records());
    MINILSM_CHECK(iterator.SeekToFirst().ok());
    const std::vector<minilsm::InternalKey> expected = {
        Value("a", 4U, "").key,
        Value(
            "b",
            std::numeric_limits<std::uint64_t>::max(),
            "")
            .key,
        Value("b", 1U, "").key,
        Tombstone("c", 3U).key,
    };
    std::size_t position = 0U;
    while (iterator.Valid()) {
        MINILSM_CHECK(position < expected.size());
        MINILSM_CHECK(KeysEqual(iterator.key(), expected[position]));
        iterator.Next();
        ++position;
    }
    MINILSM_CHECK(position == expected.size());

    const minilsm::InternalKey lower{
        "b",
        std::numeric_limits<std::uint64_t>::max(),
        minilsm::RecordType::kTombstone,
    };
    MINILSM_CHECK(iterator.Seek(lower).ok());
    MINILSM_CHECK(iterator.Valid());
    MINILSM_CHECK(iterator.key().user_key == "b");
    MINILSM_CHECK(
        iterator.key().sequence_number ==
        std::numeric_limits<std::uint64_t>::max());
    return true;
}

bool TestEmptyAndOneSourceMerge() {
    std::vector<std::unique_ptr<minilsm::InternalIterator>> empty;
    minilsm::MergingIterator empty_merge(std::move(empty));
    MINILSM_CHECK(empty_merge.SeekToFirst().ok());
    MINILSM_CHECK(!empty_merge.Valid());
    MINILSM_CHECK(empty_merge.status().ok());

    const std::vector<Entry> records = {
        Value("a", 8U, "a"),
        Value("b", 4U, "b"),
        Tombstone("c", 1U),
    };
    std::vector<std::unique_ptr<minilsm::InternalIterator>> one;
    one.push_back(Child(records));
    minilsm::MergingIterator one_merge(std::move(one));
    MINILSM_CHECK(one_merge.SeekToFirst().ok());
    std::size_t position = 0U;
    while (one_merge.Valid()) {
        MINILSM_CHECK(position < records.size());
        MINILSM_CHECK(KeysEqual(one_merge.key(), records[position].key));
        MINILSM_CHECK(one_merge.value() == records[position].value);
        one_merge.Next();
        ++position;
    }
    MINILSM_CHECK(position == records.size());
    MINILSM_CHECK(one_merge.status().ok());
    return true;
}

bool TestManySourceFullInternalKeyMerge() {
    std::vector<std::unique_ptr<minilsm::InternalIterator>> children;
    children.push_back(Child({
        Value("a", 8U, "a8"),
        Value("c", 7U, "c7"),
        Value("c", 2U, "c2"),
        Value("f", 1U, "f"),
    }));
    children.push_back(Child({
        Value("a", 10U, "a10"),
        Value("b", 5U, "b"),
        Value("c", 9U, "c9"),
        Value("e", 4U, "e"),
    }));

    const std::vector<Entry> expected = {
        Value("a", 10U, "a10"),
        Value("a", 8U, "a8"),
        Value("b", 5U, "b"),
        Value("c", 9U, "c9"),
        Value("c", 7U, "c7"),
        Value("c", 2U, "c2"),
        Value("e", 4U, "e"),
        Value("f", 1U, "f"),
    };

    minilsm::MergingIterator merge(std::move(children));
    MINILSM_CHECK(merge.SeekToFirst().ok());
    std::size_t position = 0U;
    while (merge.Valid()) {
        MINILSM_CHECK(position < expected.size());
        MINILSM_CHECK(KeysEqual(merge.key(), expected[position].key));
        MINILSM_CHECK(merge.value() == expected[position].value);
        merge.Next();
        ++position;
    }
    MINILSM_CHECK(position == expected.size());
    MINILSM_CHECK(merge.status().ok());
    return true;
}

bool TestExactDuplicateUsesStableSourceTieBreak() {
    std::vector<std::unique_ptr<minilsm::InternalIterator>> children;
    children.push_back(Child({
        Value("x", 7U, "source-zero"),
        Value("z", 1U, "z"),
    }));
    children.push_back(Child({
        Value("x", 7U, "source-one"),
        Value("y", 2U, "y"),
    }));

    minilsm::MergingIterator merge(std::move(children));
    MINILSM_CHECK(merge.SeekToFirst().ok());
    MINILSM_CHECK(merge.Valid());
    MINILSM_CHECK(merge.key().user_key == "x");
    MINILSM_CHECK(merge.value() == "source-zero");
    merge.Next();
    MINILSM_CHECK(merge.Valid());
    MINILSM_CHECK(merge.key().user_key == "x");
    MINILSM_CHECK(merge.value() == "source-one");
    merge.Next();
    MINILSM_CHECK(merge.key().user_key == "y");
    merge.Next();
    MINILSM_CHECK(merge.key().user_key == "z");
    merge.Next();
    MINILSM_CHECK(!merge.Valid());
    MINILSM_CHECK(merge.status().ok());
    return true;
}

bool TestExactDuplicateIsCollapsedByUserIterator() {
    std::vector<std::unique_ptr<minilsm::InternalIterator>> children;
    children.push_back(Child({
        Value("x", 7U, "source-zero"),
        Value("z", 1U, "z"),
    }));
    children.push_back(Child({
        Value("x", 7U, "source-one"),
        Value("y", 2U, "y"),
    }));

    auto merge =
        std::make_unique<minilsm::MergingIterator>(std::move(children));
    MINILSM_CHECK(merge->SeekToFirst().ok());
    minilsm::UserIterator user(std::move(merge));
    const std::vector<UserEntry> expected = {
        {"x", "source-zero"}, {"y", "y"}, {"z", "z"}};
    std::size_t position = 0U;
    while (user.Valid()) {
        MINILSM_CHECK(position < expected.size());
        MINILSM_CHECK(user.key() == expected[position].first);
        MINILSM_CHECK(user.value() == expected[position].second);
        user.Next();
        ++position;
    }
    MINILSM_CHECK(position == expected.size());
    MINILSM_CHECK(user.status().ok());
    return true;
}

bool TestUserVisibilityAndTombstoneContinuation() {
    std::vector<std::unique_ptr<minilsm::InternalIterator>> children;
    children.push_back(Child({
        Tombstone("a", 10U),
        Value("b", 9U, "b-new"),
        Value("c", 8U, "c-new"),
    }));
    children.push_back(Child({
        Value("a", 8U, "a-old"),
        Tombstone("b", 7U),
        Tombstone("c", 6U),
        Value("d", 5U, "d"),
    }));

    auto merge =
        std::make_unique<minilsm::MergingIterator>(std::move(children));
    MINILSM_CHECK(merge->SeekToFirst().ok());
    minilsm::UserIterator user(std::move(merge));
    const std::vector<std::pair<std::string, std::string>> expected = {
        {"b", "b-new"},
        {"c", "c-new"},
        {"d", "d"},
    };
    std::size_t position = 0U;
    while (user.Valid()) {
        MINILSM_CHECK(position < expected.size());
        MINILSM_CHECK(user.key() == expected[position].first);
        MINILSM_CHECK(user.value() == expected[position].second);
        user.Next();
        ++position;
    }
    MINILSM_CHECK(position == expected.size());
    MINILSM_CHECK(user.status().ok());
    return true;
}

bool TestLongVersionSequenceIsFullyConsumed() {
    std::vector<std::unique_ptr<minilsm::InternalIterator>> children;
    for (std::uint64_t source = 0U; source < 4U; ++source) {
        std::vector<Entry> records;
        for (std::uint64_t sequence = 100U; sequence > 0U; --sequence) {
            if (sequence % 4U == source) {
                records.push_back(Value(
                    "k", sequence, "v-" + std::to_string(sequence)));
            }
        }
        if (source == 0U) {
            records.push_back(Value("z", 1U, "after"));
        }
        children.push_back(Child(std::move(records)));
    }

    auto merge =
        std::make_unique<minilsm::MergingIterator>(std::move(children));
    MINILSM_CHECK(merge->SeekToFirst().ok());
    minilsm::UserIterator user(std::move(merge));
    MINILSM_CHECK(user.Valid());
    MINILSM_CHECK(user.key() == "k");
    MINILSM_CHECK(user.value() == "v-100");
    user.Next();
    MINILSM_CHECK(user.Valid());
    MINILSM_CHECK(user.key() == "z");
    MINILSM_CHECK(user.value() == "after");
    user.Next();
    MINILSM_CHECK(!user.Valid());
    MINILSM_CHECK(user.status().ok());
    return true;
}

bool TestChildErrorPropagatesBeforePartialUserKey() {
    std::vector<std::unique_ptr<minilsm::InternalIterator>> children;
    children.push_back(std::make_unique<VectorInternalIterator>(
        std::vector<Entry>{Value("a", 2U, "value")}, 1U));
    children.push_back(Child({Value("b", 1U, "later")}));

    auto merge =
        std::make_unique<minilsm::MergingIterator>(std::move(children));
    MINILSM_CHECK(merge->SeekToFirst().ok());
    minilsm::UserIterator user(std::move(merge));
    MINILSM_CHECK(!user.Valid());
    MINILSM_CHECK(
        user.status().code() == minilsm::Status::Code::kCorruption);
    return true;
}

}  // namespace

int main() {
    MINILSM_RUN(TestMemTableIterationAndSeek);
    MINILSM_RUN(TestEmptyAndOneSourceMerge);
    MINILSM_RUN(TestManySourceFullInternalKeyMerge);
    MINILSM_RUN(TestExactDuplicateUsesStableSourceTieBreak);
    MINILSM_RUN(TestExactDuplicateIsCollapsedByUserIterator);
    MINILSM_RUN(TestUserVisibilityAndTombstoneContinuation);
    MINILSM_RUN(TestLongVersionSequenceIsFullyConsumed);
    MINILSM_RUN(TestChildErrorPropagatesBeforePartialUserKey);
    return 0;
}
