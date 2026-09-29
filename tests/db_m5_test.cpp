#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cstdlib>
#include <unistd.h>

#include "common/file_io.hpp"
#include "common/file_names.hpp"
#include "db/db_impl.hpp"
#include "db/db_lifecycle.hpp"
#include "manifest/manifest.hpp"
#include "manifest/version.hpp"
#include "minilsm/db.hpp"
#include "minilsm/options.hpp"
#include "minilsm/status.hpp"
#include "sstable/block.hpp"
#include "sstable/sstable_builder.hpp"
#include "sstable/sstable_reader.hpp"
#include "test_harness.hpp"
#include "legacy_options.hpp"

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern =
            (std::filesystem::temp_directory_path() /
             "minilsm-m5-tests-XXXXXX")
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

bool CheckValue(
    const minilsm::LookupResult& result,
    std::string_view expected) {
    MINILSM_CHECK(result.status.ok());
    MINILSM_CHECK(result.value == std::optional<std::string>(expected));
    return true;
}

bool CheckNotFound(const minilsm::LookupResult& result) {
    MINILSM_CHECK(
        result.status.code() == minilsm::Status::Code::kNotFound);
    MINILSM_CHECK(!result.value.has_value());
    return true;
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

minilsm::FileMetaData RangeFile(
    std::uint64_t number,
    std::string smallest,
    std::string largest) {
    minilsm::FileMetaData file;
    file.file_number = number;
    file.level = 1U;
    file.file_size = minilsm::kMinSstableFileBytes;
    file.entry_count = 1U;
    file.smallest_sequence = 1U;
    file.largest_sequence = 1U;
    file.smallest_user_key = std::move(smallest);
    file.largest_user_key = std::move(largest);
    return file;
}

bool TestLowerLevelBinarySelectionBoundaries() {
    const std::vector<minilsm::FileMetaData> files = {
        RangeFile(10U, "a", "c"),
        RangeFile(11U, "g", "k"),
        RangeFile(12U, "m", "z"),
    };

    MINILSM_CHECK(!minilsm::FindFileByUserKeyRange(files, "").has_value());
    MINILSM_CHECK(minilsm::FindFileByUserKeyRange(files, "a") == 0U);
    MINILSM_CHECK(minilsm::FindFileByUserKeyRange(files, "b") == 0U);
    MINILSM_CHECK(minilsm::FindFileByUserKeyRange(files, "c") == 0U);
    MINILSM_CHECK(!minilsm::FindFileByUserKeyRange(files, "d").has_value());
    MINILSM_CHECK(minilsm::FindFileByUserKeyRange(files, "g") == 1U);
    MINILSM_CHECK(minilsm::FindFileByUserKeyRange(files, "i") == 1U);
    MINILSM_CHECK(minilsm::FindFileByUserKeyRange(files, "k") == 1U);
    MINILSM_CHECK(!minilsm::FindFileByUserKeyRange(files, "l").has_value());
    MINILSM_CHECK(minilsm::FindFileByUserKeyRange(files, "m") == 2U);
    MINILSM_CHECK(minilsm::FindFileByUserKeyRange(files, "z") == 2U);
    MINILSM_CHECK(!minilsm::FindFileByUserKeyRange(
        files, std::string(1U, static_cast<char>(0xFFU))).has_value());
    return true;
}

bool TestMemtableAndOverlappingL0Freshness() {
    TemporaryDirectory directory;
    minilsm::Options options = LegacyOptions();
    options.memtable_bytes_limit = 1024U * 1024U;
    options.l0_compaction_file_trigger =
        minilsm::kMaxManifestLiveFiles;
    std::unique_ptr<minilsm::DBImpl> db;
    MINILSM_CHECK(
        minilsm::DBImpl::Open(options, directory.path(), &db).ok());

    MINILSM_CHECK(CheckNotFound(db->Get("missing")));
    MINILSM_CHECK(db->Put("key", "old").ok());
    db->ResetReadCounters();
    MINILSM_CHECK(CheckValue(db->Get("key"), "old"));
    MINILSM_CHECK(db->read_counters().sstable_files_considered == 0U);
    MINILSM_CHECK(db->FlushForTesting().ok());

    MINILSM_CHECK(db->Put("a", "range-start").ok());
    MINILSM_CHECK(db->Put("z", "range-end").ok());
    MINILSM_CHECK(db->FlushForTesting().ok());
    db->ResetReadCounters();
    MINILSM_CHECK(CheckValue(db->Get("key"), "old"));
    MINILSM_CHECK(db->read_counters().sstable_files_considered == 2U);
    MINILSM_CHECK(db->read_counters().bloom_filter_probes == 2U);
    MINILSM_CHECK(db->read_counters().bloom_definite_negatives >= 1U);
    MINILSM_CHECK(db->read_counters().sstable_data_block_reads == 1U);

    MINILSM_CHECK(db->Put("key", "new").ok());
    MINILSM_CHECK(db->FlushForTesting().ok());
    db->ResetReadCounters();
    MINILSM_CHECK(CheckValue(db->Get("key"), "new"));
    MINILSM_CHECK(db->read_counters().sstable_files_considered == 1U);

    MINILSM_CHECK(db->Delete("key").ok());
    MINILSM_CHECK(db->FlushForTesting().ok());
    db->ResetReadCounters();
    MINILSM_CHECK(CheckNotFound(db->Get("key")));
    MINILSM_CHECK(db->read_counters().sstable_files_considered == 1U);

    MINILSM_CHECK(db->Put("key", "memtable-wins").ok());
    db->ResetReadCounters();
    MINILSM_CHECK(CheckValue(db->Get("key"), "memtable-wins"));
    MINILSM_CHECK(db->read_counters().sstable_files_considered == 0U);
    MINILSM_CHECK(db->Delete("key").ok());
    db->ResetReadCounters();
    MINILSM_CHECK(CheckNotFound(db->Get("key")));
    MINILSM_CHECK(db->read_counters().sstable_files_considered == 0U);
    MINILSM_CHECK(db->Close().ok());
    return true;
}

bool TestLowerLevelGetInspectsOneCandidatePerLevel() {
    TemporaryDirectory directory;
    std::unique_ptr<minilsm::DB> initializer;
    MINILSM_CHECK(minilsm::DB::Open(
        LegacyOptions(), directory.path(), &initializer).ok());
    MINILSM_CHECK(initializer->Close().ok());
    initializer.reset();

    minilsm::Version version;
    MINILSM_CHECK(ReadActiveVersion(directory.path(), &version));
    minilsm::FileMetaData file;
    MINILSM_CHECK(BuildTable(
        directory.path(), 10U, 1U,
        {Value("a", 100U, "a"), Value("c", 99U, "c")}, &file));
    version.levels[1].push_back(file);
    MINILSM_CHECK(BuildTable(
        directory.path(), 11U, 1U,
        {Value("shared", 90U, "from-l1")}, &file));
    version.levels[1].push_back(file);

    MINILSM_CHECK(BuildTable(
        directory.path(), 12U, 2U,
        {Value("lower-only", 80U, "from-l2")}, &file));
    version.levels[2].push_back(file);
    MINILSM_CHECK(BuildTable(
        directory.path(), 13U, 2U,
        {Value("shared", 70U, "older-l2")}, &file));
    version.levels[2].push_back(file);

    MINILSM_CHECK(BuildTable(
        directory.path(), 14U, 3U,
        {Value("deep-only", 60U, "from-l3")}, &file));
    version.levels[3].push_back(file);
    MINILSM_CHECK(BuildTable(
        directory.path(), 15U, 3U,
        {Value("shared", 50U, "oldest-l3")}, &file));
    version.levels[3].push_back(file);

    version.manifest_file_number = 20U;
    version.last_sequence = 100U;
    version.next_file_number = 21U;
    MINILSM_CHECK(minilsm::WriteManifestSnapshotFile(
        directory.path(), version, false).ok());
    bool current_replaced = false;
    MINILSM_CHECK(minilsm::InstallCurrentFile(
        directory.path(), 20U, &current_replaced, false).ok());
    MINILSM_CHECK(current_replaced);

    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(minilsm::DB::Open(
        LegacyOptions(), directory.path(), &db).ok());
    db->ResetReadCounters();
    MINILSM_CHECK(CheckValue(db->Get("shared"), "from-l1"));
    MINILSM_CHECK(db->read_counters().sstable_files_considered == 1U);
    MINILSM_CHECK(db->read_counters().sstable_data_block_reads == 1U);

    db->ResetReadCounters();
    MINILSM_CHECK(CheckValue(db->Get("lower-only"), "from-l2"));
    MINILSM_CHECK(db->read_counters().sstable_files_considered == 1U);

    db->ResetReadCounters();
    MINILSM_CHECK(CheckValue(db->Get("deep-only"), "from-l3"));
    MINILSM_CHECK(db->read_counters().sstable_files_considered == 1U);

    db->ResetReadCounters();
    MINILSM_CHECK(CheckNotFound(db->Get("range-miss")));
    MINILSM_CHECK(db->read_counters().sstable_files_considered == 0U);
    MINILSM_CHECK(db->Close().ok());
    return true;
}

bool TestPersistedFiltersAndCountersAfterReopen() {
    TemporaryDirectory directory;
    minilsm::Options options = LegacyOptions();
    options.memtable_bytes_limit = 1024U * 1024U;
    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(
        minilsm::DB::Open(options, directory.path(), &db).ok());
    MINILSM_CHECK(db->Put("a", "first").ok());
    MINILSM_CHECK(db->Put("z", "last").ok());
    MINILSM_CHECK(db->Close().ok());
    db.reset();

    MINILSM_CHECK(
        minilsm::DB::Open(options, directory.path(), &db).ok());
    db->ResetReadCounters();
    MINILSM_CHECK(CheckValue(db->Get("a"), "first"));
    MINILSM_CHECK(CheckValue(db->Get("z"), "last"));
    minilsm::ReadCounters counters = db->read_counters();
    MINILSM_CHECK(counters.bloom_filter_probes == 2U);
    MINILSM_CHECK(counters.bloom_possible_positives == 2U);
    MINILSM_CHECK(counters.sstable_data_block_reads == 2U);

    db->ResetReadCounters();
    for (std::size_t index = 0U; index < 100U; ++index) {
        MINILSM_CHECK(CheckNotFound(
            db->Get("middle-" + std::to_string(index))));
    }
    counters = db->read_counters();
    MINILSM_CHECK(counters.sstable_files_considered == 100U);
    MINILSM_CHECK(counters.bloom_filter_probes == 100U);
    MINILSM_CHECK(counters.bloom_definite_negatives > 0U);
    MINILSM_CHECK(
        counters.sstable_data_block_reads <
        counters.sstable_files_considered);
    MINILSM_CHECK(db->Close().ok());
    return true;
}

bool TestDeterministicRandomizedReferenceModel() {
    constexpr std::uint64_t kSeed = 0x4D35474554524546ULL;
    std::mt19937_64 random(kSeed);
    TemporaryDirectory directory;
    minilsm::Options options = LegacyOptions();
    options.memtable_bytes_limit = 70U;
    options.l0_compaction_file_trigger =
        minilsm::kMaxManifestLiveFiles;
    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(
        minilsm::DB::Open(options, directory.path(), &db).ok());

    std::vector<std::string> keys;
    for (std::size_t index = 0U; index < 14U; ++index) {
        keys.push_back("key-" + std::to_string(index));
    }
    keys.push_back(std::string("\0\xFF", 2U));
    std::map<std::string, std::string> reference;
    for (std::size_t operation = 0U; operation < 240U; ++operation) {
        const std::string& key =
            keys[static_cast<std::size_t>(random() % keys.size())];
        const std::uint64_t choice = random() % 3U;
        if (choice == 0U) {
            const std::string value =
                "value-" + std::to_string(random() % 100'000U);
            MINILSM_CHECK(db->Put(key, value).ok());
            reference[key] = value;
        } else if (choice == 1U) {
            MINILSM_CHECK(db->Delete(key).ok());
            reference.erase(key);
        } else {
            const auto expected = reference.find(key);
            if (expected == reference.end()) {
                MINILSM_CHECK(CheckNotFound(db->Get(key)));
            } else {
                MINILSM_CHECK(CheckValue(db->Get(key), expected->second));
            }
        }

        if ((operation + 1U) % 23U == 0U) {
            MINILSM_CHECK(db->Close().ok());
            db.reset();
            MINILSM_CHECK(
                minilsm::DB::Open(options, directory.path(), &db).ok());
            for (const std::string& known_key : keys) {
                const auto expected = reference.find(known_key);
                if (expected == reference.end()) {
                    MINILSM_CHECK(CheckNotFound(db->Get(known_key)));
                } else {
                    MINILSM_CHECK(CheckValue(
                        db->Get(known_key), expected->second));
                }
            }
        }
    }

    MINILSM_CHECK(db->Close().ok());
    db.reset();
    minilsm::Version version;
    MINILSM_CHECK(ReadActiveVersion(directory.path(), &version));
    MINILSM_CHECK(version.levels[0].size() > 20U);
    MINILSM_CHECK(
        minilsm::DB::Open(options, directory.path(), &db).ok());
    for (const std::string& key : keys) {
        const auto expected = reference.find(key);
        if (expected == reference.end()) {
            MINILSM_CHECK(CheckNotFound(db->Get(key)));
        } else {
            MINILSM_CHECK(CheckValue(db->Get(key), expected->second));
        }
    }
    MINILSM_CHECK(db->Close().ok());
    return true;
}

}  // namespace

int main() {
    MINILSM_RUN(TestLowerLevelBinarySelectionBoundaries);
    MINILSM_RUN(TestMemtableAndOverlappingL0Freshness);
    MINILSM_RUN(TestLowerLevelGetInspectsOneCandidatePerLevel);
    MINILSM_RUN(TestPersistedFiltersAndCountersAfterReopen);
    MINILSM_RUN(TestDeterministicRandomizedReferenceModel);
    return 0;
}
