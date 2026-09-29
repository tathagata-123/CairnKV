#include <cstdint>
#include <string>

#include "common/internal_key.hpp"
#include "memtable/memtable.hpp"
#include "minilsm/format_limits.hpp"
#include "minilsm/status.hpp"
#include "test_harness.hpp"

namespace {

using minilsm::InternalKey;
using minilsm::MemTable;
using minilsm::MemTableLookupState;
using minilsm::RecordType;

bool TestPutGetOverwriteAndAbsent() {
    MemTable memtable;
    MINILSM_CHECK(memtable.size() == 0U);
    MINILSM_CHECK(memtable.empty());
    MINILSM_CHECK(memtable.approximate_memory_usage() == 0U);
    MINILSM_CHECK(
        memtable.Lookup("absent").state ==
        MemTableLookupState::kNotPresent);

    MINILSM_CHECK(
        memtable
            .Add(
                InternalKey{"key", 1U, RecordType::kValue},
                "first")
            .ok());
    MINILSM_CHECK(memtable.size() == 1U);
    MINILSM_CHECK(!memtable.empty());
    MINILSM_CHECK(
        memtable.approximate_memory_usage() ==
        minilsm::kMinInternalKeyBytes + 3U + 5U);
    minilsm::MemTableLookup found = memtable.Lookup("key");
    MINILSM_CHECK(found.state == MemTableLookupState::kValue);
    MINILSM_CHECK(found.value == "first");

    MINILSM_CHECK(
        memtable
            .Add(
                InternalKey{"key", 2U, RecordType::kValue},
                "second")
            .ok());
    found = memtable.Lookup("key");
    MINILSM_CHECK(found.state == MemTableLookupState::kValue);
    MINILSM_CHECK(found.value == "second");
    MINILSM_CHECK(memtable.size() == 2U);
    MINILSM_CHECK(memtable.records().size() == 2U);
    memtable.Clear();
    MINILSM_CHECK(memtable.empty());
    MINILSM_CHECK(memtable.approximate_memory_usage() == 0U);
    return true;
}

bool TestNewestSequenceWinsRegardlessOfInsertionOrder() {
    MemTable memtable;
    MINILSM_CHECK(
        memtable
            .Add(
                InternalKey{"versions", 8U, RecordType::kValue},
                "eight")
            .ok());
    MINILSM_CHECK(
        memtable
            .Add(
                InternalKey{"versions", 3U, RecordType::kValue},
                "three")
            .ok());
    MINILSM_CHECK(
        memtable
            .Add(
                InternalKey{"versions", 12U, RecordType::kValue},
                "twelve")
            .ok());
    const minilsm::MemTableLookup found =
        memtable.Lookup("versions");
    MINILSM_CHECK(found.state == MemTableLookupState::kValue);
    MINILSM_CHECK(found.value == "twelve");
    return true;
}

bool TestDeleteAndResurrection() {
    MemTable memtable;
    MINILSM_CHECK(
        memtable
            .Add(
                InternalKey{"key", 1U, RecordType::kValue},
                "old")
            .ok());
    MINILSM_CHECK(
        memtable
            .Add(
                InternalKey{"key", 2U, RecordType::kTombstone},
                "")
            .ok());
    MINILSM_CHECK(
        memtable.Lookup("key").state ==
        MemTableLookupState::kTombstone);

    MINILSM_CHECK(
        memtable
            .Add(
                InternalKey{
                    "missing", 3U, RecordType::kTombstone},
                "")
            .ok());
    MINILSM_CHECK(
        memtable.Lookup("missing").state ==
        MemTableLookupState::kTombstone);

    MINILSM_CHECK(
        memtable
            .Add(
                InternalKey{"key", 4U, RecordType::kValue},
                "new")
            .ok());
    const minilsm::MemTableLookup found = memtable.Lookup("key");
    MINILSM_CHECK(found.state == MemTableLookupState::kValue);
    MINILSM_CHECK(found.value == "new");
    MINILSM_CHECK(memtable.size() == 4U);
    return true;
}

bool TestBinaryKeysValuesAndInvalidRecords() {
    MemTable memtable;
    const std::string key("\0\x80\xFF", 3U);
    const std::string value("\0value\xFF", 7U);
    MINILSM_CHECK(
        memtable
            .Add(
                InternalKey{key, 1U, RecordType::kValue},
                value)
            .ok());
    const minilsm::MemTableLookup found = memtable.Lookup(key);
    MINILSM_CHECK(found.state == MemTableLookupState::kValue);
    MINILSM_CHECK(found.value == value);

    MINILSM_CHECK(
        memtable
            .Add(
                InternalKey{key, 1U, RecordType::kTombstone},
                "")
            .code() == minilsm::Status::Code::kCorruption);
    MINILSM_CHECK(
        memtable
            .Add(
                InternalKey{"bad", 0U, RecordType::kValue},
                "value")
            .code() ==
        minilsm::Status::Code::kInvalidArgument);
    MINILSM_CHECK(
        memtable
            .Add(
                InternalKey{
                    "bad", 2U, RecordType::kTombstone},
                "not empty")
            .code() ==
        minilsm::Status::Code::kInvalidArgument);
    return true;
}

}  // namespace

int main() {
    MINILSM_RUN(TestPutGetOverwriteAndAbsent);
    MINILSM_RUN(TestNewestSequenceWinsRegardlessOfInsertionOrder);
    MINILSM_RUN(TestDeleteAndResurrection);
    MINILSM_RUN(TestBinaryKeysValuesAndInvalidRecords);
    return 0;
}
