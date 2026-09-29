#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <cstdlib>
#include <unistd.h>

#include "common/coding.hpp"
#include "common/internal_key.hpp"
#include "minilsm/status.hpp"
#include "sstable/block.hpp"
#include "sstable/sstable_builder.hpp"
#include "sstable/sstable_iterator.hpp"
#include "sstable/sstable_reader.hpp"
#include "test_harness.hpp"

namespace {

using minilsm::DataBlockEntry;
using minilsm::InternalKey;
using minilsm::InternalKeyComparator;
using minilsm::RecordType;

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern =
            (std::filesystem::temp_directory_path() /
             "minilsm-sstable-random-XXXXXX")
                .string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        if (created != nullptr) {
            path_ = created;
        }
    }

    ~TemporaryDirectory() {
        if (!path_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

bool KeysEqual(const InternalKey& left, const InternalKey& right) {
    return left.user_key == right.user_key &&
           left.sequence_number == right.sequence_number &&
           left.record_type == right.record_type;
}

std::size_t ReferenceLowerBound(
    const std::vector<DataBlockEntry>& records,
    const InternalKey& target) {
    const InternalKeyComparator compare;
    const auto found = std::lower_bound(
        records.begin(),
        records.end(),
        target,
        [&compare](const DataBlockEntry& entry, const InternalKey& key) {
            return compare(entry.internal_key, key);
        });
    return static_cast<std::size_t>(found - records.begin());
}

bool TestDeterministicRandomizedReference() {
    constexpr std::uint64_t kSeed = 0x4D3353535441424CULL;
    std::mt19937_64 random(kSeed);
    std::vector<DataBlockEntry> records;
    records.reserve(240U);

    // Appending the unique numeric suffix makes uniqueness explicit rather
    // than probabilistic while the random prefix still exercises binary keys.
    for (std::uint32_t index = 0U; index < 240U; ++index) {
        std::string user_key(8U, '\0');
        for (std::size_t byte = 0U; byte < 4U; ++byte) {
            user_key[byte] = static_cast<char>(random() & 0xFFU);
        }
        std::string suffix;
        minilsm::PutFixed32(&suffix, index);
        user_key.replace(4U, 4U, suffix);

        std::uint64_t sequence = random();
        if (sequence == 0U) {
            sequence = 1U;
        }
        const RecordType type =
            (random() % 5U == 0U)
                ? RecordType::kTombstone
                : RecordType::kValue;
        std::string value;
        if (type == RecordType::kValue) {
            value = std::string("value\0", 6U) +
                    std::to_string(random() % 1'000'000U);
        }
        records.push_back(DataBlockEntry{
            InternalKey{std::move(user_key), sequence, type},
            std::move(value),
        });
    }

    const InternalKeyComparator compare;
    std::sort(
        records.begin(),
        records.end(),
        [&compare](const DataBlockEntry& left, const DataBlockEntry& right) {
            return compare(left.internal_key, right.internal_key);
        });

    TemporaryDirectory directory;
    const std::filesystem::path path = directory.path() / "random.sst";
    std::unique_ptr<minilsm::SstableBuilder> builder;
    MINILSM_CHECK(
        minilsm::SstableBuilder::Create(path, 1024U, &builder).ok());
    for (const DataBlockEntry& record : records) {
        MINILSM_CHECK(
            builder->Add(record.internal_key, record.value).ok());
    }
    MINILSM_CHECK(builder->Finish().ok());

    std::unique_ptr<minilsm::SstableReader> reader;
    MINILSM_CHECK(minilsm::SstableReader::Open(path, &reader).ok());
    MINILSM_CHECK(reader->entry_count() == records.size());
    MINILSM_CHECK(reader->index().size() > 1U);

    minilsm::SstableIterator iterator(reader.get());
    MINILSM_CHECK(iterator.SeekToFirst().ok());
    std::size_t position = 0U;
    while (iterator.Valid()) {
        MINILSM_CHECK(position < records.size());
        MINILSM_CHECK(
            KeysEqual(iterator.key(), records[position].internal_key));
        MINILSM_CHECK(iterator.value() == records[position].value);
        iterator.Next();
        ++position;
    }
    MINILSM_CHECK(iterator.status().ok());
    MINILSM_CHECK(position == records.size());

    for (const DataBlockEntry& record : records) {
        const minilsm::LookupResult found =
            reader->Get(record.internal_key);
        MINILSM_CHECK(found.status.ok());
        MINILSM_CHECK(found.value.has_value());
        MINILSM_CHECK(*found.value == record.value);
    }

    for (std::size_t index = 0U; index < 20U; ++index) {
        std::string absent_key(20U, static_cast<char>(0xFFU));
        absent_key.push_back(static_cast<char>(index));
        const minilsm::LookupResult missing = reader->Get(
            InternalKey{absent_key, index + 1U, RecordType::kValue});
        MINILSM_CHECK(
            missing.status.code() == minilsm::Status::Code::kNotFound);
    }

    for (std::size_t index = 0U; index < records.size(); index += 23U) {
        MINILSM_CHECK(iterator.Seek(records[index].internal_key).ok());
        MINILSM_CHECK(iterator.Valid());
        MINILSM_CHECK(KeysEqual(iterator.key(), records[index].internal_key));

        InternalKey between = records[index].internal_key;
        between.user_key.push_back('\0');
        const std::size_t expected = ReferenceLowerBound(records, between);
        MINILSM_CHECK(iterator.Seek(between).ok());
        if (expected == records.size()) {
            MINILSM_CHECK(!iterator.Valid());
        } else {
            MINILSM_CHECK(iterator.Valid());
            MINILSM_CHECK(
                KeysEqual(iterator.key(), records[expected].internal_key));
        }
    }

    const InternalKey before_first{"", 1U, RecordType::kValue};
    MINILSM_CHECK(iterator.Seek(before_first).ok());
    MINILSM_CHECK(iterator.Valid());
    MINILSM_CHECK(KeysEqual(iterator.key(), records.front().internal_key));

    const InternalKey after_last{
        std::string(32U, static_cast<char>(0xFFU)),
        1U,
        RecordType::kValue,
    };
    MINILSM_CHECK(iterator.Seek(after_last).ok());
    MINILSM_CHECK(!iterator.Valid());
    MINILSM_CHECK(iterator.status().ok());
    return true;
}

}  // namespace

int main() {
    MINILSM_RUN(TestDeterministicRandomizedReference);
    return 0;
}
