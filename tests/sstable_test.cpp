#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cstdlib>
#include <unistd.h>

#include "common/file_io.hpp"
#include "common/internal_key.hpp"
#include "minilsm/status.hpp"
#include "sstable/block.hpp"
#include "sstable/sstable_builder.hpp"
#include "sstable/sstable_iterator.hpp"
#include "sstable/sstable_reader.hpp"
#include "test_harness.hpp"

namespace {

using minilsm::DataBlockEntry;
using minilsm::DataBlockReader;
using minilsm::InternalKey;
using minilsm::RecordType;
using minilsm::SstableBuilder;
using minilsm::SstableIterator;
using minilsm::SstableReader;

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern =
            (std::filesystem::temp_directory_path() /
             "minilsm-sstable-tests-XXXXXX")
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

InternalKey Key(
    std::string user_key,
    std::uint64_t sequence,
    RecordType type = RecordType::kValue) {
    return InternalKey{std::move(user_key), sequence, type};
}

bool KeysEqual(const InternalKey& left, const InternalKey& right) {
    return left.user_key == right.user_key &&
           left.sequence_number == right.sequence_number &&
           left.record_type == right.record_type;
}

bool BuildTable(
    const std::filesystem::path& path,
    std::size_t target_size,
    const std::vector<DataBlockEntry>& records) {
    std::unique_ptr<SstableBuilder> builder;
    MINILSM_CHECK(
        SstableBuilder::Create(path, target_size, &builder).ok());
    for (const DataBlockEntry& record : records) {
        MINILSM_CHECK(
            builder->Add(record.internal_key, record.value).ok());
    }
    MINILSM_CHECK(builder->Finish().ok());
    return true;
}

bool ReadWholeFile(
    const std::filesystem::path& path,
    std::string* bytes) {
    std::uint64_t size = 0U;
    MINILSM_CHECK(minilsm::GetFileSize(path, &size).ok());
    std::unique_ptr<minilsm::RandomAccessFileReader> reader;
    MINILSM_CHECK(
        minilsm::RandomAccessFileReader::Open(path, &reader).ok());
    MINILSM_CHECK(
        reader->Read(0U, static_cast<std::size_t>(size), bytes).ok());
    MINILSM_CHECK(reader->Close().ok());
    return true;
}

bool WriteWholeFile(
    const std::filesystem::path& path,
    std::string_view bytes) {
    std::unique_ptr<minilsm::SequentialFileWriter> writer;
    MINILSM_CHECK(
        minilsm::SequentialFileWriter::CreateNew(path, &writer).ok());
    MINILSM_CHECK(writer->Append(bytes).ok());
    MINILSM_CHECK(writer->Close().ok());
    return true;
}

bool CheckFound(
    const SstableReader& reader,
    const InternalKey& key,
    std::string_view expected_value) {
    const minilsm::LookupResult result = reader.Get(key);
    MINILSM_CHECK(result.status.ok());
    MINILSM_CHECK(result.value.has_value());
    MINILSM_CHECK(*result.value == expected_value);
    return true;
}

bool CheckMissing(
    const SstableReader& reader,
    const InternalKey& key) {
    const minilsm::LookupResult result = reader.Get(key);
    MINILSM_CHECK(
        result.status.code() == minilsm::Status::Code::kNotFound);
    MINILSM_CHECK(!result.value.has_value());
    return true;
}

bool TestEmptyPolicyAndOneEntryTable() {
    TemporaryDirectory directory;
    MINILSM_CHECK(!directory.path().empty());

    const std::filesystem::path empty_path = directory.path() / "empty.sst";
    std::unique_ptr<SstableBuilder> empty_builder;
    MINILSM_CHECK(
        SstableBuilder::Create(empty_path, 4096U, &empty_builder).ok());
    MINILSM_CHECK(
        empty_builder->Finish().code() ==
        minilsm::Status::Code::kInvalidArgument);
    empty_builder.reset();
    std::unique_ptr<SstableReader> empty_reader;
    MINILSM_CHECK(
        SstableReader::Open(empty_path, &empty_reader).code() ==
        minilsm::Status::Code::kCorruption);

    const std::filesystem::path one_path = directory.path() / "one.sst";
    const DataBlockEntry record{Key("only", 5U), "value"};
    MINILSM_CHECK(BuildTable(one_path, 4096U, {record}));

    std::unique_ptr<SstableReader> reader;
    MINILSM_CHECK(SstableReader::Open(one_path, &reader).ok());
    MINILSM_CHECK(reader->entry_count() == 1U);
    MINILSM_CHECK(reader->index().size() == 1U);
    MINILSM_CHECK(reader->has_bloom_filter());
    MINILSM_CHECK(reader->footer().filter_offset ==
                   reader->footer().index_offset +
                       reader->footer().index_size);
    MINILSM_CHECK(reader->footer().filter_size == 48U);
    MINILSM_CHECK(reader->bloom_filter_key_count() == 1U);
    MINILSM_CHECK(reader->MayContain("only"));
    MINILSM_CHECK(CheckFound(*reader, record.internal_key, "value"));
    minilsm::SstableProperties properties;
    MINILSM_CHECK(reader->ValidateAndReadProperties(&properties).ok());
    MINILSM_CHECK(
        properties.file_size >=
        minilsm::kMinSstableFileBytes + 48U);
    MINILSM_CHECK(properties.entry_count == 1U);
    MINILSM_CHECK(properties.smallest_sequence == 5U);
    MINILSM_CHECK(properties.largest_sequence == 5U);
    MINILSM_CHECK(
        KeysEqual(properties.smallest_internal_key, record.internal_key));
    MINILSM_CHECK(
        KeysEqual(properties.largest_internal_key, record.internal_key));
    return true;
}

bool TestPreM5AbsentFilterCompatibility() {
    TemporaryDirectory directory;
    const DataBlockEntry record{Key("legacy", 5U), "value"};
    const std::filesystem::path filtered =
        directory.path() / "filtered.sst";
    MINILSM_CHECK(BuildTable(filtered, 4096U, {record}));

    std::unique_ptr<SstableReader> filtered_reader;
    MINILSM_CHECK(SstableReader::Open(filtered, &filtered_reader).ok());
    minilsm::SstableFooter legacy_footer = filtered_reader->footer();
    MINILSM_CHECK(legacy_footer.filter_offset != 0U);
    MINILSM_CHECK(legacy_footer.filter_size != 0U);

    std::string bytes;
    MINILSM_CHECK(ReadWholeFile(filtered, &bytes));
    bytes.erase(
        static_cast<std::size_t>(legacy_footer.filter_offset),
        static_cast<std::size_t>(legacy_footer.filter_size));
    legacy_footer.filter_offset = 0U;
    legacy_footer.filter_size = 0U;
    std::string encoded_footer;
    MINILSM_CHECK(
        minilsm::EncodeSstableFooter(
            legacy_footer, &encoded_footer).ok());
    bytes.replace(
        bytes.size() - minilsm::kSstableFooterBytes,
        minilsm::kSstableFooterBytes,
        encoded_footer);

    const std::filesystem::path legacy = directory.path() / "legacy.sst";
    MINILSM_CHECK(WriteWholeFile(legacy, bytes));
    std::unique_ptr<SstableReader> reader;
    MINILSM_CHECK(SstableReader::Open(legacy, &reader).ok());
    MINILSM_CHECK(!reader->has_bloom_filter());
    MINILSM_CHECK(reader->bloom_filter_key_count() == 0U);
    MINILSM_CHECK(reader->MayContain("legacy"));
    MINILSM_CHECK(reader->MayContain("definitely-not-stored"));
    MINILSM_CHECK(CheckFound(*reader, record.internal_key, "value"));
    return true;
}

bool TestTargetBoundaryTransitionAndOversizedEntry() {
    TemporaryDirectory directory;

    const std::filesystem::path exact_path = directory.path() / "exact.sst";
    const std::vector<DataBlockEntry> exact_records = {
        {Key("a", 1U), std::string(982U, 'a')},
        {Key("b", 1U), "next"},
    };
    MINILSM_CHECK(BuildTable(exact_path, 1024U, exact_records));
    std::unique_ptr<SstableReader> reader;
    MINILSM_CHECK(SstableReader::Open(exact_path, &reader).ok());
    MINILSM_CHECK(reader->index().size() == 2U);
    MINILSM_CHECK(reader->index()[0].block_size == 1024U);
    MINILSM_CHECK(reader->index()[1].block_offset == 1024U);

    const std::filesystem::path transition_path =
        directory.path() / "transition.sst";
    const std::vector<DataBlockEntry> transition_records = {
        {Key("a", 1U), std::string(500U, 'a')},
        {Key("b", 1U), std::string(500U, 'b')},
        {Key("c", 1U), std::string(500U, 'c')},
    };
    MINILSM_CHECK(BuildTable(transition_path, 1024U, transition_records));
    MINILSM_CHECK(SstableReader::Open(transition_path, &reader).ok());
    MINILSM_CHECK(reader->index().size() == 3U);

    const std::filesystem::path oversized_path =
        directory.path() / "oversized.sst";
    const std::vector<DataBlockEntry> oversized_records = {
        {Key("large", 1U), std::string(2000U, 'v')},
        {Key("later", 1U), "small"},
    };
    MINILSM_CHECK(BuildTable(oversized_path, 1024U, oversized_records));
    MINILSM_CHECK(SstableReader::Open(oversized_path, &reader).ok());
    MINILSM_CHECK(reader->index().size() == 2U);
    MINILSM_CHECK(reader->index()[0].block_size > 1024U);
    DataBlockReader block;
    MINILSM_CHECK(reader->ReadDataBlock(0U, &block).ok());
    MINILSM_CHECK(block.entries().size() == 1U);
    MINILSM_CHECK(block.entries()[0].value.size() == 2000U);
    return true;
}

bool TestBuilderRejectsDuplicateAndDecreasingKeys() {
    TemporaryDirectory directory;
    {
        std::unique_ptr<SstableBuilder> builder;
        MINILSM_CHECK(
            SstableBuilder::Create(
                directory.path() / "duplicate.sst", 1024U, &builder)
                .ok());
        MINILSM_CHECK(builder->Add(Key("key", 7U), "one").ok());
        MINILSM_CHECK(
            builder->Add(Key("key", 7U), "two").code() ==
            minilsm::Status::Code::kInvalidArgument);
    }
    {
        std::unique_ptr<SstableBuilder> builder;
        MINILSM_CHECK(
            SstableBuilder::Create(
                directory.path() / "decreasing.sst", 1024U, &builder)
                .ok());
        MINILSM_CHECK(builder->Add(Key("z", 1U), "one").ok());
        MINILSM_CHECK(
            builder->Add(Key("a", 1U), "two").code() ==
            minilsm::Status::Code::kInvalidArgument);
    }
    {
        std::unique_ptr<SstableBuilder> builder;
        MINILSM_CHECK(
            SstableBuilder::Create(
                directory.path() / "same-sequence.sst", 1024U, &builder)
                .ok());
        MINILSM_CHECK(
            builder
                ->Add(Key("key", 9U, RecordType::kTombstone), "")
                .ok());
        MINILSM_CHECK(
            builder->Add(Key("key", 9U), "value").code() ==
            minilsm::Status::Code::kInvalidArgument);
    }
    return true;
}

std::vector<DataBlockEntry> OrderedRecords() {
    return {
        {Key("a", 9U), "a-new"},
        {Key("a", 2U), "a-old"},
        {Key("c", 8U, RecordType::kTombstone), ""},
        {Key("e", 7U), "e-value"},
        {Key("g", 6U), "g-value"},
        {Key("i", 5U), "i-value"},
    };
}

bool TestPointLookupAndSparseIndex() {
    TemporaryDirectory directory;
    const std::vector<DataBlockEntry> records = OrderedRecords();
    const std::filesystem::path path = directory.path() / "many.sst";
    MINILSM_CHECK(BuildTable(path, 1024U, records));

    std::unique_ptr<SstableReader> reader;
    MINILSM_CHECK(SstableReader::Open(path, &reader).ok());
    MINILSM_CHECK(reader->entry_count() == records.size());
    MINILSM_CHECK(CheckFound(*reader, records.front().internal_key, "a-new"));
    MINILSM_CHECK(CheckFound(*reader, records[3].internal_key, "e-value"));
    MINILSM_CHECK(CheckFound(*reader, records.back().internal_key, "i-value"));
    MINILSM_CHECK(CheckFound(*reader, records[2].internal_key, ""));
    MINILSM_CHECK(CheckMissing(*reader, Key("", 1U)));
    MINILSM_CHECK(CheckMissing(*reader, Key("b", 1U)));
    MINILSM_CHECK(CheckMissing(*reader, Key("z", 1U)));

    std::uint64_t expected_offset = 0U;
    for (std::size_t block_index = 0U;
         block_index < reader->index().size();
         ++block_index) {
        const minilsm::BlockIndexEntry& index_entry =
            reader->index()[block_index];
        MINILSM_CHECK(index_entry.block_offset == expected_offset);
        MINILSM_CHECK(index_entry.block_size >= 41U);
        DataBlockReader block;
        MINILSM_CHECK(reader->ReadDataBlock(block_index, &block).ok());
        MINILSM_CHECK(!block.entries().empty());
        MINILSM_CHECK(KeysEqual(
            block.entries().back().internal_key,
            index_entry.largest_internal_key));
        expected_offset += index_entry.block_size;
    }
    MINILSM_CHECK(expected_offset == reader->footer().index_offset);
    return true;
}

bool TestForwardIterationAndSeek() {
    TemporaryDirectory directory;
    const std::vector<DataBlockEntry> records = OrderedRecords();
    const std::filesystem::path path = directory.path() / "iterate.sst";
    MINILSM_CHECK(BuildTable(path, 1024U, records));
    std::unique_ptr<SstableReader> reader;
    MINILSM_CHECK(SstableReader::Open(path, &reader).ok());

    SstableIterator iterator(reader.get());
    MINILSM_CHECK(iterator.SeekToFirst().ok());
    std::size_t index = 0U;
    while (iterator.Valid()) {
        MINILSM_CHECK(index < records.size());
        MINILSM_CHECK(KeysEqual(iterator.key(), records[index].internal_key));
        MINILSM_CHECK(iterator.value() == records[index].value);
        iterator.Next();
        ++index;
    }
    MINILSM_CHECK(iterator.status().ok());
    MINILSM_CHECK(index == records.size());

    MINILSM_CHECK(iterator.Seek(records.front().internal_key).ok());
    MINILSM_CHECK(iterator.Valid());
    MINILSM_CHECK(KeysEqual(iterator.key(), records.front().internal_key));

    MINILSM_CHECK(iterator.Seek(records.back().internal_key).ok());
    MINILSM_CHECK(iterator.Valid());
    MINILSM_CHECK(KeysEqual(iterator.key(), records.back().internal_key));

    MINILSM_CHECK(iterator.Seek(Key("d", 1U)).ok());
    MINILSM_CHECK(iterator.Valid());
    MINILSM_CHECK(KeysEqual(iterator.key(), records[3].internal_key));

    MINILSM_CHECK(iterator.Seek(Key("", 1U)).ok());
    MINILSM_CHECK(iterator.Valid());
    MINILSM_CHECK(KeysEqual(iterator.key(), records.front().internal_key));

    MINILSM_CHECK(iterator.Seek(Key("z", 1U)).ok());
    MINILSM_CHECK(!iterator.Valid());
    MINILSM_CHECK(iterator.status().ok());
    return true;
}

bool TestDeterministicOutput() {
    TemporaryDirectory directory;
    const std::vector<DataBlockEntry> records = OrderedRecords();
    const std::filesystem::path first = directory.path() / "first.sst";
    const std::filesystem::path second = directory.path() / "second.sst";
    MINILSM_CHECK(BuildTable(first, 1024U, records));
    MINILSM_CHECK(BuildTable(second, 1024U, records));
    std::string first_bytes;
    std::string second_bytes;
    MINILSM_CHECK(ReadWholeFile(first, &first_bytes));
    MINILSM_CHECK(ReadWholeFile(second, &second_bytes));
    MINILSM_CHECK(first_bytes == second_bytes);
    return true;
}

}  // namespace

int main() {
    MINILSM_RUN(TestEmptyPolicyAndOneEntryTable);
    MINILSM_RUN(TestPreM5AbsentFilterCompatibility);
    MINILSM_RUN(TestTargetBoundaryTransitionAndOversizedEntry);
    MINILSM_RUN(TestBuilderRejectsDuplicateAndDecreasingKeys);
    MINILSM_RUN(TestPointLookupAndSparseIndex);
    MINILSM_RUN(TestForwardIterationAndSeek);
    MINILSM_RUN(TestDeterministicOutput);
    return 0;
}
