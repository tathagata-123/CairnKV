#include <algorithm>
#include <string>
#include <vector>
#include "common/checksum.hpp"
#include "common/coding.hpp"
#include "sstable/block.hpp"
#include "test_harness.hpp"

namespace {
using namespace minilsm;

void Store(std::string* bytes, std::size_t offset, std::uint32_t value) {
    std::string field; PutFixed32(&field, value); bytes->replace(offset, 4, field);
}
void Refresh(std::string* bytes) {
    Store(bytes, bytes->size() - 4, Crc32c(std::string_view(*bytes).substr(0, bytes->size() - 4)));
}

bool TestCompressionAndRestartSeeking() {
    DataBlockBuilder compressed(true), legacy;
    std::vector<InternalKey> keys;
    for (int i = 0; i < 70; ++i) {
        const std::string user = "customer:common-prefix:" + std::to_string(100000 + i);
        keys.push_back({user, 100, RecordType::kValue});
        if (i % 7 == 0) keys.push_back({user, 90, RecordType::kTombstone});
    }
    for (const auto& key : keys) {
        const std::string value = key.record_type == RecordType::kTombstone ? "" : "value";
        MINILSM_CHECK(compressed.Add(key, value).ok());
        MINILSM_CHECK(legacy.Add(key, value).ok());
    }
    std::string bytes, old;
    MINILSM_CHECK(compressed.Finish(&bytes).ok());
    MINILSM_CHECK(legacy.Finish(&old).ok());
    MINILSM_CHECK(bytes.size() < old.size());
    DataBlockReader block;
    MINILSM_CHECK(DataBlockReader::Decode(bytes, &block).ok());
    MINILSM_CHECK(block.entries().size() == keys.size());
    std::vector<InternalKey> probes = keys;
    probes.push_back({"", 1, RecordType::kValue});
    probes.push_back({"zzzz", 1, RecordType::kValue});
    for (const auto& key : keys) {
        probes.push_back({key.user_key, 95, RecordType::kValue});
        probes.push_back({key.user_key + "x", 1, RecordType::kValue});
    }
    for (const auto& probe : probes) {
        const auto expected = std::lower_bound(keys.begin(), keys.end(), probe, InternalKeyComparator{});
        MINILSM_CHECK(block.LowerBound(probe) == static_cast<std::size_t>(expected - keys.begin()));
    }
    return true;
}

bool TestNoPrefixAndMalformedRestarts() {
    DataBlockBuilder builder(true);
    for (int i = 0; i < 33; ++i) {
        const std::string key(1, static_cast<char>(i + 1));
        MINILSM_CHECK(builder.Add({key, 1, RecordType::kValue}, "").ok());
    }
    std::string bytes;
    MINILSM_CHECK(builder.Finish(&bytes).ok());
    DataBlockReader reader;
    MINILSM_CHECK(DataBlockReader::Decode(bytes, &reader).ok());
    const std::size_t restart_table = bytes.size() - 8 - 3 * 4;
    std::vector<std::string> malformed(5, bytes);
    Store(&malformed[0], bytes.size() - 8, 0); // no restarts
    Store(&malformed[1], restart_table, 1); // first restart must be zero
    Store(&malformed[2], restart_table + 4, 1); // not an entry boundary
    Store(&malformed[3], 16, 1); // first entry cannot share
    Store(&malformed[4], 20, 0xffffffffU); // suffix outside block
    for (auto& bad : malformed) {
        Refresh(&bad);
        MINILSM_CHECK(!DataBlockReader::Decode(bad, &reader).ok());
    }
    for (std::size_t n = 0; n < bytes.size(); ++n)
        MINILSM_CHECK(!DataBlockReader::Decode(std::string_view(bytes).substr(0, n), &reader).ok());
    DataBlockBuilder one(true);
    MINILSM_CHECK(one.Add({"", 1, RecordType::kValue}, "").ok());
    MINILSM_CHECK(one.Finish(&bytes).ok());
    MINILSM_CHECK(DataBlockReader::Decode(bytes, &reader).ok());
    MINILSM_CHECK(reader.LowerBound({"", 1, RecordType::kValue}) == 0);
    MINILSM_CHECK(reader.LowerBound({"a", 1, RecordType::kValue}) == 1);
    return true;
}
}  // namespace

int main() {
    MINILSM_RUN(TestCompressionAndRestartSeeking);
    MINILSM_RUN(TestNoPrefixAndMalformedRestarts);
    return 0;
}
