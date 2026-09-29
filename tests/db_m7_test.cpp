#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cstdlib>
#include <unistd.h>

#include "common/file_io.hpp"
#include "common/file_names.hpp"
#include "common/internal_key.hpp"
#include "compaction/compaction.hpp"
#include "db/db_impl.hpp"
#include "db/db_lifecycle.hpp"
#include "manifest/manifest.hpp"
#include "manifest/version.hpp"
#include "minilsm/db.hpp"
#include "minilsm/format_limits.hpp"
#include "minilsm/iterator.hpp"
#include "minilsm/options.hpp"
#include "minilsm/status.hpp"
#include "sstable/block.hpp"
#include "sstable/sstable_builder.hpp"
#include "sstable/sstable_iterator.hpp"
#include "sstable/sstable_reader.hpp"
#include "test_harness.hpp"
#include "legacy_options.hpp"

namespace {

using UserEntry = std::pair<std::string, std::string>;

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern =
            (std::filesystem::temp_directory_path() /
             "minilsm-m7-tests-XXXXXX")
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

std::filesystem::path NumberedPath(
    const std::filesystem::path& directory,
    minilsm::FileKind kind,
    std::uint64_t number) {
    std::string name;
    minilsm::Status status = minilsm::Status::InvalidArgument("kind");
    if (kind == minilsm::FileKind::kWal) {
        status = minilsm::MakeWalFileName(number, false, &name);
    } else if (kind == minilsm::FileKind::kSstable) {
        status = minilsm::MakeSstableFileName(number, false, &name);
    } else if (kind == minilsm::FileKind::kManifest) {
        status = minilsm::MakeManifestFileName(number, false, &name);
    }
    if (!status.ok()) {
        return {};
    }
    return directory / name;
}

bool ReadWholeFile(
    const std::filesystem::path& path,
    std::string* bytes) {
    std::uint64_t size = 0U;
    MINILSM_CHECK(minilsm::GetFileSize(path, &size).ok());
    std::unique_ptr<minilsm::RandomAccessFileReader> reader;
    MINILSM_CHECK(
        minilsm::RandomAccessFileReader::Open(path, &reader).ok());
    MINILSM_CHECK(reader->Read(
        0U, static_cast<std::size_t>(size), bytes).ok());
    MINILSM_CHECK(reader->Close().ok());
    return true;
}

bool ReadActiveVersion(
    const std::filesystem::path& directory,
    minilsm::Version* version) {
    std::string current;
    MINILSM_CHECK(ReadWholeFile(directory / "CURRENT", &current));
    std::uint64_t manifest_number = 0U;
    MINILSM_CHECK(
        minilsm::DecodeCurrent(current, &manifest_number).ok());
    std::string manifest;
    MINILSM_CHECK(ReadWholeFile(
        NumberedPath(
            directory, minilsm::FileKind::kManifest, manifest_number),
        &manifest));
    MINILSM_CHECK(minilsm::DecodeManifestSnapshot(
        manifest, manifest_number, version).ok());
    return true;
}

minilsm::DataBlockEntry Value(
    std::string user_key,
    std::uint64_t sequence,
    std::string value) {
    return minilsm::DataBlockEntry{
        minilsm::InternalKey{
            std::move(user_key),
            sequence,
            minilsm::RecordType::kValue},
        std::move(value),
    };
}

bool BuildTable(
    const std::filesystem::path& directory,
    std::uint64_t file_number,
    std::uint32_t level,
    const std::vector<minilsm::DataBlockEntry>& entries,
    minilsm::FileMetaData* metadata) {
    const std::filesystem::path path = NumberedPath(
        directory, minilsm::FileKind::kSstable, file_number);
    std::unique_ptr<minilsm::SstableBuilder> builder;
    MINILSM_CHECK(
        minilsm::SstableBuilder::Create(path, 1024U, &builder).ok());
    for (const minilsm::DataBlockEntry& entry : entries) {
        MINILSM_CHECK(
            builder->Add(entry.internal_key, entry.value).ok());
    }
    MINILSM_CHECK(builder->Finish().ok());

    std::unique_ptr<minilsm::SstableReader> reader;
    MINILSM_CHECK(minilsm::SstableReader::Open(path, &reader).ok());
    minilsm::SstableProperties properties;
    MINILSM_CHECK(reader->ValidateAndReadProperties(&properties).ok());
    metadata->file_number = file_number;
    metadata->level = level;
    metadata->file_size = properties.file_size;
    metadata->entry_count = properties.entry_count;
    metadata->smallest_sequence = properties.smallest_sequence;
    metadata->largest_sequence = properties.largest_sequence;
    metadata->smallest_user_key =
        properties.smallest_internal_key.user_key;
    metadata->largest_user_key =
        properties.largest_internal_key.user_key;
    metadata->smallest_internal_key = properties.smallest_internal_key;
    metadata->largest_internal_key = properties.largest_internal_key;
    return true;
}

bool ReadTableEntries(
    const std::filesystem::path& directory,
    const minilsm::FileMetaData& file,
    std::vector<minilsm::DataBlockEntry>* entries) {
    std::unique_ptr<minilsm::SstableReader> reader;
    MINILSM_CHECK(minilsm::SstableReader::Open(
        NumberedPath(
            directory, minilsm::FileKind::kSstable, file.file_number),
        &reader).ok());
    minilsm::SstableProperties properties;
    MINILSM_CHECK(reader->ValidateAndReadProperties(&properties).ok());
    MINILSM_CHECK(reader->has_bloom_filter());

    minilsm::SstableIterator iterator(reader.get());
    MINILSM_CHECK(iterator.SeekToFirst().ok());
    std::vector<minilsm::DataBlockEntry> result;
    while (iterator.Valid()) {
        result.push_back(minilsm::DataBlockEntry{
            iterator.key(), std::string(iterator.value())});
        iterator.Next();
    }
    MINILSM_CHECK(iterator.status().ok());
    *entries = std::move(result);
    return true;
}

bool ExpectScan(
    const minilsm::DB& db,
    const std::vector<UserEntry>& expected) {
    std::unique_ptr<minilsm::Iterator> iterator =
        db.Scan(std::nullopt, std::nullopt);
    std::size_t position = 0U;
    while (iterator->Valid()) {
        MINILSM_CHECK(position < expected.size());
        MINILSM_CHECK(iterator->key() == expected[position].first);
        MINILSM_CHECK(iterator->value() == expected[position].second);
        iterator->Next();
        ++position;
    }
    MINILSM_CHECK(iterator->status().ok());
    MINILSM_CHECK(position == expected.size());
    return true;
}

bool CheckValue(
    const minilsm::DB& db,
    std::string_view key,
    std::string_view value) {
    const minilsm::LookupResult result = db.Get(key);
    MINILSM_CHECK(result.status.ok());
    MINILSM_CHECK(result.value == std::optional<std::string>(value));
    return true;
}

bool CheckNotFound(const minilsm::DB& db, std::string_view key) {
    const minilsm::LookupResult result = db.Get(key);
    MINILSM_CHECK(
        result.status.code() == minilsm::Status::Code::kNotFound);
    MINILSM_CHECK(!result.value.has_value());
    return true;
}

minilsm::FileMetaData Meta(
    std::uint64_t file_number,
    std::uint32_t level,
    std::string smallest,
    std::string largest,
    std::uint64_t sequence) {
    minilsm::FileMetaData file;
    file.file_number = file_number;
    file.level = level;
    file.file_size = 200U;
    file.entry_count = 1U;
    file.smallest_sequence = sequence;
    file.largest_sequence = sequence;
    file.smallest_user_key = std::move(smallest);
    file.largest_user_key = std::move(largest);
    return file;
}

bool TestPickerAndDirectNonOverlapValidator() {
    minilsm::Version version;
    version.manifest_file_number = 90U;
    version.active_wal_file_number = 91U;
    version.last_sequence = 100U;
    version.next_file_number = 1000U;
    version.levels[0] = {
        Meta(10U, 0U, "b", "d", 90U),
        Meta(11U, 0U, "f", "h", 100U),
    };
    version.levels[1] = {
        Meta(20U, 1U, "a", "b", 80U),
        Meta(21U, 1U, "e", "g", 70U),
        Meta(22U, 1U, "i", "k", 60U),
    };
    version.levels[2] = {
        Meta(30U, 2U, "a", "a", 50U),
        Meta(31U, 2U, "b", "c", 40U),
        Meta(32U, 2U, "h", "j", 30U),
    };
    version.levels[3] = {
        Meta(40U, 3U, "a", "b", 20U),
        Meta(41U, 3U, "k", "z", 10U),
    };
    MINILSM_CHECK(minilsm::ValidateVersionMetadata(version).ok());

    minilsm::Options options = LegacyOptions();
    options.l0_compaction_file_trigger = 2U;
    options.l1_max_bytes = 1000U;
    options.l2_max_bytes = 1000U;
    std::optional<minilsm::CompactionPlan> plan;
    MINILSM_CHECK(minilsm::PickCompaction(options, version, &plan).ok());
    MINILSM_CHECK(plan.has_value());
    MINILSM_CHECK(plan->source_level == 0U);
    MINILSM_CHECK(plan->destination_level == 1U);
    MINILSM_CHECK(plan->source_files.size() == 2U);
    MINILSM_CHECK(plan->overlapping_destination_files.size() == 2U);
    MINILSM_CHECK(plan->source_smallest_user_key == "b");
    MINILSM_CHECK(plan->source_largest_user_key == "h");
    MINILSM_CHECK(plan->smallest_user_key == "a");
    MINILSM_CHECK(plan->largest_user_key == "h");

    version.levels[0].clear();
    version.last_sequence = 80U;
    options.l0_compaction_file_trigger = 99U;
    options.l1_max_bytes = 599U;
    MINILSM_CHECK(minilsm::PickCompaction(options, version, &plan).ok());
    MINILSM_CHECK(plan->source_level == 1U);
    MINILSM_CHECK(plan->source_files.size() == 1U);
    MINILSM_CHECK(plan->source_files[0].file_number == 20U);
    MINILSM_CHECK(plan->overlapping_destination_files.size() == 2U);
    MINILSM_CHECK(
        plan->overlapping_destination_files[0].file_number == 30U);
    MINILSM_CHECK(
        plan->overlapping_destination_files[1].file_number == 31U);

    version.levels[1].clear();
    version.last_sequence = 50U;
    options.l1_max_bytes = 1000U;
    options.l2_max_bytes = 599U;
    MINILSM_CHECK(minilsm::PickCompaction(options, version, &plan).ok());
    MINILSM_CHECK(plan->source_level == 2U);
    MINILSM_CHECK(plan->source_files[0].file_number == 30U);
    MINILSM_CHECK(plan->overlapping_destination_files.size() == 1U);
    MINILSM_CHECK(
        plan->overlapping_destination_files[0].file_number == 40U);

    const std::string byte_7f(1U, static_cast<char>(0x7FU));
    const std::string byte_80(1U, static_cast<char>(0x80U));
    std::vector<minilsm::FileMetaData> binary = {
        Meta(1U, 1U, byte_7f, byte_7f, 1U),
        Meta(2U, 1U, byte_80, byte_80, 1U),
    };
    MINILSM_CHECK(minilsm::ValidateLevelNonOverlap(binary, 1U).ok());
    binary[1].smallest_user_key = byte_7f;
    MINILSM_CHECK(
        minilsm::ValidateLevelNonOverlap(binary, 1U).code() ==
        minilsm::Status::Code::kCorruption);
    return true;
}

bool TestL0OverlapSelectionPreservesUnrelatedL1File() {
    TemporaryDirectory directory;
    std::unique_ptr<minilsm::DB> initializer;
    MINILSM_CHECK(minilsm::DB::Open(
        LegacyOptions(), directory.path(), &initializer).ok());
    MINILSM_CHECK(initializer->Close().ok());
    initializer.reset();

    minilsm::Version version;
    MINILSM_CHECK(ReadActiveVersion(directory.path(), &version));
    minilsm::FileMetaData overlapping;
    minilsm::FileMetaData unrelated;
    MINILSM_CHECK(BuildTable(
        directory.path(),
        10U,
        1U,
        {Value("a", 2U, "old-a"), Value("c", 1U, "old-c")},
        &overlapping));
    MINILSM_CHECK(BuildTable(
        directory.path(),
        11U,
        1U,
        {Value("z", 3U, "unrelated")},
        &unrelated));
    version.levels[1] = {overlapping, unrelated};
    version.manifest_file_number = 12U;
    version.last_sequence = 3U;
    version.next_file_number = 13U;
    MINILSM_CHECK(minilsm::WriteManifestSnapshotFile(
        directory.path(), version, false).ok());
    bool current_replaced = false;
    MINILSM_CHECK(minilsm::InstallCurrentFile(
        directory.path(), 12U, &current_replaced, false).ok());
    MINILSM_CHECK(current_replaced);

    minilsm::Options options = LegacyOptions();
    options.memtable_bytes_limit = 1024U * 1024U;
    options.l0_compaction_file_trigger = 2U;
    options.l1_max_bytes = minilsm::kMaxDatabaseFileBytes;
    options.l2_max_bytes = minilsm::kMaxDatabaseFileBytes;
    std::unique_ptr<minilsm::DBImpl> db;
    MINILSM_CHECK(minilsm::DBImpl::Open(
        options, directory.path(), &db).ok());
    MINILSM_CHECK(db->Put("b", "new-b").ok());
    MINILSM_CHECK(db->FlushForTesting().ok());
    MINILSM_CHECK(db->Put("d", "new-d").ok());
    MINILSM_CHECK(db->FlushForTesting().ok());

    minilsm::Version compacted;
    MINILSM_CHECK(ReadActiveVersion(directory.path(), &compacted));
    MINILSM_CHECK(compacted.levels[0].empty());
    MINILSM_CHECK(compacted.levels[1].size() == 2U);
    MINILSM_CHECK(
        minilsm::ValidateLevelNonOverlap(compacted.levels[1], 1U).ok());
    MINILSM_CHECK(compacted.levels[1][1].file_number == 11U);
    MINILSM_CHECK(compacted.levels[1][1].smallest_user_key == "z");
    bool exists = true;
    MINILSM_CHECK(minilsm::FileExists(
        NumberedPath(
            directory.path(), minilsm::FileKind::kSstable, 10U),
        &exists).ok());
    MINILSM_CHECK(!exists);
    MINILSM_CHECK(minilsm::FileExists(
        NumberedPath(
            directory.path(), minilsm::FileKind::kSstable, 11U),
        &exists).ok());
    MINILSM_CHECK(exists);
    MINILSM_CHECK(db->Get("a").value ==
                   std::optional<std::string>("old-a"));
    MINILSM_CHECK(db->Get("b").value ==
                   std::optional<std::string>("new-b"));
    MINILSM_CHECK(db->Get("c").value ==
                   std::optional<std::string>("old-c"));
    MINILSM_CHECK(db->Get("d").value ==
                   std::optional<std::string>("new-d"));
    MINILSM_CHECK(db->Get("z").value ==
                   std::optional<std::string>("unrelated"));
    MINILSM_CHECK(db->Close().ok());
    db.reset();
    std::unique_ptr<minilsm::DB> reopened;
    MINILSM_CHECK(minilsm::DB::Open(
        options, directory.path(), &reopened).ok());
    MINILSM_CHECK(ExpectScan(
        *reopened,
        {{"a", "old-a"},
         {"b", "new-b"},
         {"c", "old-c"},
         {"d", "new-d"},
         {"z", "unrelated"}}));
    MINILSM_CHECK(reopened->Close().ok());
    return true;
}

bool TestOutputSplittingOverwriteAndCounters() {
    TemporaryDirectory directory;
    minilsm::Options options = LegacyOptions();
    options.memtable_bytes_limit = 1024U * 1024U;
    options.l0_compaction_file_trigger = 2U;
    options.l1_max_bytes = minilsm::kMaxDatabaseFileBytes;
    options.l2_max_bytes = minilsm::kMaxDatabaseFileBytes;
    options.compaction_output_file_target_bytes = 90U;
    std::unique_ptr<minilsm::DBImpl> db;
    MINILSM_CHECK(minilsm::DBImpl::Open(
        options, directory.path(), &db).ok());

    std::map<std::string, std::string> reference;
    for (std::size_t index = 0U; index < 10U; ++index) {
        const std::string key = "key-" + std::to_string(index);
        const std::string value(32U, static_cast<char>('a' + index));
        MINILSM_CHECK(db->Put(key, value).ok());
        reference[key] = value;
    }
    MINILSM_CHECK(db->FlushForTesting().ok());
    minilsm::Version first_flush;
    MINILSM_CHECK(ReadActiveVersion(directory.path(), &first_flush));
    MINILSM_CHECK(first_flush.levels[0].size() == 1U);
    const std::uint64_t first_input =
        first_flush.levels[0][0].file_number;
    const std::uint64_t second_input = first_flush.next_file_number;

    for (std::size_t index = 0U; index < 10U; index += 2U) {
        const std::string key = "key-" + std::to_string(index);
        const std::string value(32U, static_cast<char>('A' + index));
        MINILSM_CHECK(db->Put(key, value).ok());
        reference[key] = value;
    }
    MINILSM_CHECK(db->FlushForTesting().ok());

    minilsm::Version compacted;
    MINILSM_CHECK(ReadActiveVersion(directory.path(), &compacted));
    MINILSM_CHECK(compacted.levels[0].empty());
    MINILSM_CHECK(compacted.levels[1].size() > 1U);
    MINILSM_CHECK(
        minilsm::ValidateLevelNonOverlap(compacted.levels[1], 1U).ok());

    std::map<std::string, std::size_t> key_counts;
    for (std::size_t file_index = 0U;
         file_index < compacted.levels[1].size();
         ++file_index) {
        const minilsm::FileMetaData& file =
            compacted.levels[1][file_index];
        std::vector<minilsm::DataBlockEntry> entries;
        MINILSM_CHECK(ReadTableEntries(
            directory.path(), file, &entries));
        std::size_t logical_bytes = 0U;
        for (const minilsm::DataBlockEntry& entry : entries) {
            ++key_counts[entry.internal_key.user_key];
            logical_bytes += 8U + minilsm::kMinInternalKeyBytes +
                             entry.internal_key.user_key.size() +
                             entry.value.size();
        }
        if (file_index + 1U < compacted.levels[1].size()) {
            MINILSM_CHECK(
                logical_bytes >=
                options.compaction_output_file_target_bytes);
        }
    }
    MINILSM_CHECK(key_counts.size() == reference.size());
    for (const auto& [key, value] : reference) {
        MINILSM_CHECK(key_counts[key] == 1U);
        MINILSM_CHECK(db->Get(key).value ==
                       std::optional<std::string>(value));
    }

    const minilsm::CompactionCounters counters =
        db->compaction_counters();
    MINILSM_CHECK(counters.bytes_read > 0U);
    MINILSM_CHECK(counters.bytes_written > 0U);
    MINILSM_CHECK(
        counters.output_file_count == compacted.levels[1].size());
    db->ResetCompactionCounters();
    MINILSM_CHECK(db->compaction_counters().bytes_read == 0U);

    bool exists = true;
    MINILSM_CHECK(minilsm::FileExists(
        NumberedPath(
            directory.path(), minilsm::FileKind::kSstable, first_input),
        &exists).ok());
    MINILSM_CHECK(!exists);
    MINILSM_CHECK(minilsm::FileExists(
        NumberedPath(
            directory.path(), minilsm::FileKind::kSstable, second_input),
        &exists).ok());
    MINILSM_CHECK(!exists);
    MINILSM_CHECK(db->Close().ok());
    db.reset();
    std::unique_ptr<minilsm::DB> reopened;
    MINILSM_CHECK(minilsm::DB::Open(
        options, directory.path(), &reopened).ok());
    MINILSM_CHECK(ExpectScan(
        *reopened,
        std::vector<UserEntry>(reference.begin(), reference.end())));
    MINILSM_CHECK(reopened->Close().ok());
    return true;
}

bool TestL1ByteTriggerOutputsToL2() {
    TemporaryDirectory directory;
    minilsm::Options options = LegacyOptions();
    options.memtable_bytes_limit = 1024U * 1024U;
    options.l0_compaction_file_trigger = 1U;
    options.l1_max_bytes = 1U;
    options.l2_max_bytes = minilsm::kMaxDatabaseFileBytes;
    std::unique_ptr<minilsm::DBImpl> db;
    MINILSM_CHECK(minilsm::DBImpl::Open(
        options, directory.path(), &db).ok());
    MINILSM_CHECK(db->Put("a", "one").ok());
    MINILSM_CHECK(db->Put("b", "two").ok());
    MINILSM_CHECK(db->FlushForTesting().ok());

    minilsm::Version version;
    MINILSM_CHECK(ReadActiveVersion(directory.path(), &version));
    MINILSM_CHECK(version.levels[0].empty());
    MINILSM_CHECK(version.levels[1].empty());
    MINILSM_CHECK(version.levels[2].size() == 1U);
    MINILSM_CHECK(version.levels[3].empty());
    MINILSM_CHECK(
        minilsm::ValidateLevelNonOverlap(version.levels[2], 2U).ok());
    MINILSM_CHECK(db->Get("a").value ==
                   std::optional<std::string>("one"));
    MINILSM_CHECK(db->Get("b").value ==
                   std::optional<std::string>("two"));
    MINILSM_CHECK(db->Close().ok());
    return true;
}

bool TestNonBottomTombstoneRetentionAndResurrection() {
    TemporaryDirectory directory;
    minilsm::Options options = LegacyOptions();
    options.memtable_bytes_limit = 1024U * 1024U;
    options.l0_compaction_file_trigger = 1U;
    options.l1_max_bytes = minilsm::kMaxDatabaseFileBytes;
    options.l2_max_bytes = minilsm::kMaxDatabaseFileBytes;
    std::unique_ptr<minilsm::DBImpl> db;
    MINILSM_CHECK(minilsm::DBImpl::Open(
        options, directory.path(), &db).ok());

    MINILSM_CHECK(db->Put("k", "old").ok());
    MINILSM_CHECK(db->FlushForTesting().ok());
    MINILSM_CHECK(db->Delete("k").ok());
    MINILSM_CHECK(db->FlushForTesting().ok());

    minilsm::Version deleted;
    MINILSM_CHECK(ReadActiveVersion(directory.path(), &deleted));
    MINILSM_CHECK(deleted.levels[0].empty());
    MINILSM_CHECK(deleted.levels[1].size() == 1U);
    std::vector<minilsm::DataBlockEntry> entries;
    MINILSM_CHECK(ReadTableEntries(
        directory.path(), deleted.levels[1][0], &entries));
    MINILSM_CHECK(entries.size() == 1U);
    MINILSM_CHECK(
        entries[0].internal_key.record_type ==
        minilsm::RecordType::kTombstone);
    MINILSM_CHECK(
        db->Get("k").status.code() == minilsm::Status::Code::kNotFound);

    MINILSM_CHECK(db->Put("k", "new").ok());
    MINILSM_CHECK(db->FlushForTesting().ok());
    MINILSM_CHECK(db->Get("k").value ==
                   std::optional<std::string>("new"));
    MINILSM_CHECK(db->Close().ok());
    db.reset();

    std::unique_ptr<minilsm::DB> reopened;
    MINILSM_CHECK(minilsm::DB::Open(
        options, directory.path(), &reopened).ok());
    MINILSM_CHECK(CheckValue(*reopened, "k", "new"));
    MINILSM_CHECK(ExpectScan(*reopened, {{"k", "new"}}));
    MINILSM_CHECK(reopened->Close().ok());
    return true;
}

bool TestL1L2CascadeAndSafeBottomTombstoneDrop() {
    TemporaryDirectory directory;
    minilsm::Options options = LegacyOptions();
    options.memtable_bytes_limit = 1024U * 1024U;
    options.l0_compaction_file_trigger = 1U;
    options.l1_max_bytes = 1U;
    options.l2_max_bytes = 1U;
    options.compaction_output_file_target_bytes = 1024U;
    std::unique_ptr<minilsm::DBImpl> db;
    MINILSM_CHECK(minilsm::DBImpl::Open(
        options, directory.path(), &db).ok());

    MINILSM_CHECK(db->Put("k", "old").ok());
    MINILSM_CHECK(db->FlushForTesting().ok());
    minilsm::Version first;
    MINILSM_CHECK(ReadActiveVersion(directory.path(), &first));
    MINILSM_CHECK(first.levels[0].empty());
    MINILSM_CHECK(first.levels[1].empty());
    MINILSM_CHECK(first.levels[2].empty());
    MINILSM_CHECK(first.levels[3].size() == 1U);

    MINILSM_CHECK(db->Delete("k").ok());
    MINILSM_CHECK(db->Put("z", "anchor").ok());
    MINILSM_CHECK(db->FlushForTesting().ok());
    minilsm::Version second;
    MINILSM_CHECK(ReadActiveVersion(directory.path(), &second));
    MINILSM_CHECK(second.levels[0].empty());
    MINILSM_CHECK(second.levels[1].empty());
    MINILSM_CHECK(second.levels[2].empty());
    MINILSM_CHECK(
        minilsm::ValidateLevelNonOverlap(second.levels[3], 3U).ok());

    std::set<std::string> stored_keys;
    for (const minilsm::FileMetaData& file : second.levels[3]) {
        std::vector<minilsm::DataBlockEntry> entries;
        MINILSM_CHECK(ReadTableEntries(
            directory.path(), file, &entries));
        for (const minilsm::DataBlockEntry& entry : entries) {
            stored_keys.insert(entry.internal_key.user_key);
        }
    }
    MINILSM_CHECK(!stored_keys.contains("k"));
    MINILSM_CHECK(stored_keys.contains("z"));
    MINILSM_CHECK(
        db->Get("k").status.code() == minilsm::Status::Code::kNotFound);
    MINILSM_CHECK(db->Get("z").value ==
                   std::optional<std::string>("anchor"));
    MINILSM_CHECK(db->compaction_counters().output_file_count >= 5U);
    MINILSM_CHECK(db->Close().ok());
    db.reset();
    std::unique_ptr<minilsm::DB> reopened;
    MINILSM_CHECK(minilsm::DB::Open(
        options, directory.path(), &reopened).ok());
    MINILSM_CHECK(CheckNotFound(*reopened, "k"));
    MINILSM_CHECK(ExpectScan(*reopened, {{"z", "anchor"}}));
    MINILSM_CHECK(reopened->Close().ok());
    return true;
}

std::size_t CountFinalSstables(
    const std::filesystem::path& directory) {
    std::vector<std::string> names;
    if (!minilsm::ListDirectory(directory, &names).ok()) {
        return std::numeric_limits<std::size_t>::max();
    }
    std::size_t count = 0U;
    for (const std::string& name : names) {
        minilsm::ParsedFileName parsed;
        if (minilsm::ParseFileName(name, &parsed).ok() &&
            parsed.kind == minilsm::FileKind::kSstable &&
            !parsed.temporary) {
            ++count;
        }
    }
    return count;
}

bool TestPreCurrentFailuresKeepInputsAndRecoveryCleansOrphans() {
    for (std::size_t failure = 0U; failure < 2U; ++failure) {
        TemporaryDirectory directory;
        minilsm::Options options = LegacyOptions();
        options.memtable_bytes_limit = 1024U * 1024U;
        options.l0_compaction_file_trigger = 2U;
        options.l1_max_bytes = minilsm::kMaxDatabaseFileBytes;
        options.l2_max_bytes = minilsm::kMaxDatabaseFileBytes;
        std::unique_ptr<minilsm::DBImpl> db;
        MINILSM_CHECK(minilsm::DBImpl::Open(
            options, directory.path(), &db).ok());
        MINILSM_CHECK(db->Put("a", "one").ok());
        MINILSM_CHECK(db->FlushForTesting().ok());
        if (failure == 0U) {
            db->FailNextCompactionBeforeManifestForTesting();
        } else {
            db->FailNextCompactionCurrentInstallForTesting();
        }
        MINILSM_CHECK(db->Put("b", "two").ok());
        MINILSM_CHECK(
            db->FlushForTesting().code() ==
            minilsm::Status::Code::kIOError);

        minilsm::Version live;
        MINILSM_CHECK(ReadActiveVersion(directory.path(), &live));
        MINILSM_CHECK(live.levels[0].size() == 2U);
        MINILSM_CHECK(live.levels[1].empty());
        MINILSM_CHECK(CountFinalSstables(directory.path()) > 2U);
        for (const minilsm::FileMetaData& input : live.levels[0]) {
            bool exists = false;
            MINILSM_CHECK(minilsm::FileExists(
                NumberedPath(
                    directory.path(),
                    minilsm::FileKind::kSstable,
                    input.file_number),
                &exists).ok());
            MINILSM_CHECK(exists);
        }
        MINILSM_CHECK(db->compaction_counters().bytes_read == 0U);

        // Destruction without Close models the crash state. CURRENT still
        // names the input Version; completed outputs are only orphans.
        db.reset();
        std::unique_ptr<minilsm::DB> reopened;
        MINILSM_CHECK(minilsm::DB::Open(
            options, directory.path(), &reopened).ok());
        MINILSM_CHECK(CheckValue(*reopened, "a", "one"));
        MINILSM_CHECK(CheckValue(*reopened, "b", "two"));
        MINILSM_CHECK(CountFinalSstables(directory.path()) == 2U);
        MINILSM_CHECK(reopened->Close().ok());
    }
    return true;
}

std::vector<UserEntry> ReferenceEntries(
    const std::map<std::string, std::string>& reference) {
    return std::vector<UserEntry>(reference.begin(), reference.end());
}

bool ValidateLiveLevels(
    const std::filesystem::path& directory,
    bool* saw_l3) {
    minilsm::Version version;
    MINILSM_CHECK(ReadActiveVersion(directory, &version));
    MINILSM_CHECK(minilsm::ValidateVersionMetadata(version).ok());
    for (std::uint32_t level = 1U; level < minilsm::kLevelCount; ++level) {
        MINILSM_CHECK(minilsm::ValidateLevelNonOverlap(
            version.levels[level], level).ok());
    }
    if (!version.levels[3].empty()) {
        *saw_l3 = true;
    }
    return true;
}

bool TestDeterministicRandomizedGetScanAndReopen() {
    constexpr std::uint64_t kSeed = 0x4D37434F4D504143ULL;
    std::mt19937_64 random(kSeed);
    TemporaryDirectory directory;
    minilsm::Options options = LegacyOptions();
    options.memtable_bytes_limit = 70U;
    options.l0_compaction_file_trigger = 2U;
    options.l1_max_bytes = 1U;
    options.l2_max_bytes = 1U;
    options.compaction_output_file_target_bytes = 120U;
    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(minilsm::DB::Open(
        options, directory.path(), &db).ok());

    std::vector<std::string> keys;
    for (std::size_t index = 0U; index < 12U; ++index) {
        keys.push_back("key-" + std::to_string(index));
    }
    std::map<std::string, std::string> reference;
    bool saw_l3 = false;
    for (std::size_t operation = 0U; operation < 180U; ++operation) {
        const std::string& key =
            keys[static_cast<std::size_t>(random() % keys.size())];
        const std::uint64_t choice = random() % 5U;
        if (choice < 3U) {
            const std::string value =
                "value-" + std::to_string(operation) + "-" +
                std::to_string(random() % 10'000U);
            MINILSM_CHECK(db->Put(key, value).ok());
            reference[key] = value;
        } else if (choice == 3U) {
            MINILSM_CHECK(db->Delete(key).ok());
            reference.erase(key);
        } else {
            const auto expected = reference.find(key);
            if (expected == reference.end()) {
                MINILSM_CHECK(CheckNotFound(*db, key));
            } else {
                MINILSM_CHECK(CheckValue(*db, key, expected->second));
            }
        }

        MINILSM_CHECK(ValidateLiveLevels(directory.path(), &saw_l3));
        if ((operation + 1U) % 7U == 0U) {
            MINILSM_CHECK(ExpectScan(*db, ReferenceEntries(reference)));
        }
        if ((operation + 1U) % 23U == 0U) {
            MINILSM_CHECK(db->Close().ok());
            db.reset();
            MINILSM_CHECK(minilsm::DB::Open(
                options, directory.path(), &db).ok());
            MINILSM_CHECK(ExpectScan(*db, ReferenceEntries(reference)));
            for (const std::string& known_key : keys) {
                const auto expected = reference.find(known_key);
                if (expected == reference.end()) {
                    MINILSM_CHECK(CheckNotFound(*db, known_key));
                } else {
                    MINILSM_CHECK(CheckValue(
                        *db, known_key, expected->second));
                }
            }
        }
    }

    MINILSM_CHECK(db->Put("resurrect", "old").ok());
    reference["resurrect"] = "old";
    MINILSM_CHECK(db->Delete("resurrect").ok());
    reference.erase("resurrect");
    MINILSM_CHECK(db->Put("resurrect", "new").ok());
    reference["resurrect"] = "new";
    MINILSM_CHECK(db->Close().ok());
    db.reset();
    MINILSM_CHECK(minilsm::DB::Open(
        options, directory.path(), &db).ok());
    MINILSM_CHECK(ExpectScan(*db, ReferenceEntries(reference)));
    MINILSM_CHECK(CheckValue(*db, "resurrect", "new"));
    MINILSM_CHECK(ValidateLiveLevels(directory.path(), &saw_l3));
    MINILSM_CHECK(saw_l3);
    MINILSM_CHECK(db->Close().ok());
    return true;
}

}  // namespace

int main() {
    MINILSM_RUN(TestPickerAndDirectNonOverlapValidator);
    MINILSM_RUN(TestL0OverlapSelectionPreservesUnrelatedL1File);
    MINILSM_RUN(TestOutputSplittingOverwriteAndCounters);
    MINILSM_RUN(TestNonBottomTombstoneRetentionAndResurrection);
    MINILSM_RUN(TestL1ByteTriggerOutputsToL2);
    MINILSM_RUN(TestL1L2CascadeAndSafeBottomTombstoneDrop);
    MINILSM_RUN(TestPreCurrentFailuresKeepInputsAndRecoveryCleansOrphans);
    MINILSM_RUN(TestDeterministicRandomizedGetScanAndReopen);
    return 0;
}
