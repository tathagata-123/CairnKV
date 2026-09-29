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
#include <fcntl.h>
#include <unistd.h>

#include "common/file_io.hpp"
#include "common/file_names.hpp"
#include "common/internal_key.hpp"
#include "db/db_impl.hpp"
#include "manifest/manifest.hpp"
#include "manifest/version.hpp"
#include "minilsm/db.hpp"
#include "minilsm/format_limits.hpp"
#include "minilsm/iterator.hpp"
#include "minilsm/options.hpp"
#include "minilsm/status.hpp"
#include "sstable/block.hpp"
#include "sstable/sstable_builder.hpp"
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
             "minilsm-m6-tests-XXXXXX")
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
    if (kind == minilsm::FileKind::kSstable) {
        status = minilsm::MakeSstableFileName(number, false, &name);
    } else if (kind == minilsm::FileKind::kManifest) {
        status = minilsm::MakeManifestFileName(number, false, &name);
    } else if (kind == minilsm::FileKind::kWal) {
        status = minilsm::MakeWalFileName(number, false, &name);
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

minilsm::DataBlockEntry TableValue(
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

bool ExpectEntries(
    std::unique_ptr<minilsm::Iterator> iterator,
    const std::vector<UserEntry>& expected) {
    MINILSM_CHECK(iterator != nullptr);
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

bool TestPublicHalfOpenBoundsAndBinaryKeys() {
    TemporaryDirectory directory;
    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(
        minilsm::DB::Open(
            LegacyOptions(), directory.path(), &db)
            .ok());

    const std::string byte_7f(1U, static_cast<char>(0x7FU));
    const std::string byte_80(1U, static_cast<char>(0x80U));
    const std::string byte_ff(1U, static_cast<char>(0xFFU));
    const std::vector<UserEntry> all = {
        {"", "empty"},
        {"a", "va"},
        {"b", "vb"},
        {"c", "vc"},
        {"d", "vd"},
        {byte_7f, "v7f"},
        {byte_80, "v80"},
        {byte_ff, "vff"},
    };
    for (const UserEntry& entry : all) {
        MINILSM_CHECK(db->Put(entry.first, entry.second).ok());
    }

    MINILSM_CHECK(ExpectEntries(
        db->Scan(std::nullopt, std::nullopt), all));
    MINILSM_CHECK(ExpectEntries(
        db->Scan(std::string("b"), std::string("d")),
        {{"b", "vb"}, {"c", "vc"}}));
    MINILSM_CHECK(ExpectEntries(
        db->Scan(std::string("bb"), std::string("z")),
        {{"c", "vc"}, {"d", "vd"}}));
    MINILSM_CHECK(ExpectEntries(
        db->Scan(std::nullopt, std::string("c")),
        {{"", "empty"}, {"a", "va"}, {"b", "vb"}}));
    MINILSM_CHECK(ExpectEntries(
        db->Scan(std::string("c"), std::nullopt),
        {{"c", "vc"},
         {"d", "vd"},
         {byte_7f, "v7f"},
         {byte_80, "v80"},
         {byte_ff, "vff"}}));
    MINILSM_CHECK(ExpectEntries(
        db->Scan(byte_80, byte_ff), {{byte_80, "v80"}}));
    MINILSM_CHECK(ExpectEntries(
        db->Scan(std::string("b"), std::string("b")), {}));
    MINILSM_CHECK(ExpectEntries(
        db->Scan(std::string("d"), std::string("b")), {}));

    const std::string oversized(
        static_cast<std::size_t>(minilsm::kMaxUserKeyBytes) + 1U,
        'x');
    std::unique_ptr<minilsm::Iterator> invalid =
        db->Scan(oversized, std::nullopt);
    MINILSM_CHECK(!invalid->Valid());
    MINILSM_CHECK(
        invalid->status().code() ==
        minilsm::Status::Code::kInvalidArgument);

    MINILSM_CHECK(db->Close().ok());
    std::unique_ptr<minilsm::Iterator> closed =
        db->Scan(std::nullopt, std::nullopt);
    MINILSM_CHECK(!closed->Valid());
    MINILSM_CHECK(
        closed->status().code() == minilsm::Status::Code::kIOError);
    return true;
}

bool TestVisibilityAcrossMemtableAndManyL0Files() {
    TemporaryDirectory directory;
    minilsm::Options options = LegacyOptions();
    options.memtable_bytes_limit = 1024U * 1024U;
    options.l0_compaction_file_trigger =
        minilsm::kMaxManifestLiveFiles;
    std::unique_ptr<minilsm::DBImpl> db;
    MINILSM_CHECK(
        minilsm::DBImpl::Open(options, directory.path(), &db).ok());

    MINILSM_CHECK(db->Put("a", "a-old").ok());
    MINILSM_CHECK(db->FlushForTesting().ok());
    MINILSM_CHECK(db->Put("b", "b-old").ok());
    MINILSM_CHECK(db->FlushForTesting().ok());
    MINILSM_CHECK(db->Delete("c").ok());
    MINILSM_CHECK(db->FlushForTesting().ok());
    for (std::uint64_t version = 1U; version <= 20U; ++version) {
        MINILSM_CHECK(db->Put(
            "d", "d-" + std::to_string(version)).ok());
        MINILSM_CHECK(db->FlushForTesting().ok());
    }

    MINILSM_CHECK(db->Put("f", "f-old").ok());
    MINILSM_CHECK(db->FlushForTesting().ok());
    MINILSM_CHECK(db->Delete("f").ok());
    MINILSM_CHECK(db->FlushForTesting().ok());
    MINILSM_CHECK(db->Put("f", "f-newest-l0").ok());
    MINILSM_CHECK(db->FlushForTesting().ok());
    MINILSM_CHECK(db->Put("g", "g-old").ok());
    MINILSM_CHECK(db->FlushForTesting().ok());
    MINILSM_CHECK(db->Delete("g").ok());
    MINILSM_CHECK(db->FlushForTesting().ok());

    MINILSM_CHECK(db->Put("a", "a-new").ok());
    MINILSM_CHECK(db->Delete("b").ok());
    MINILSM_CHECK(db->Put("c", "c-new").ok());
    MINILSM_CHECK(db->Put("d", "d-21").ok());
    MINILSM_CHECK(db->Put("e", "after-tombstone").ok());

    MINILSM_CHECK(ExpectEntries(
        db->Scan(std::nullopt, std::nullopt),
        {{"a", "a-new"},
         {"c", "c-new"},
         {"d", "d-21"},
         {"e", "after-tombstone"},
         {"f", "f-newest-l0"}}));
    MINILSM_CHECK(ExpectEntries(
        db->Scan(std::string("d"), std::string("f")),
        {{"d", "d-21"}, {"e", "after-tombstone"}}));
    MINILSM_CHECK(db->Close().ok());
    return true;
}

minilsm::FileMetaData RangeFile(
    std::uint64_t file_number,
    std::string smallest,
    std::string largest) {
    minilsm::FileMetaData file;
    file.file_number = file_number;
    file.smallest_user_key = std::move(smallest);
    file.largest_user_key = std::move(largest);
    return file;
}

std::vector<std::uint64_t> SelectOverlapping(
    const std::vector<minilsm::FileMetaData>& files,
    const std::optional<std::string>& lower,
    const std::optional<std::string>& upper) {
    std::vector<std::uint64_t> selected;
    for (const minilsm::FileMetaData& file : files) {
        if (minilsm::SstableOverlapsUserRange(file, lower, upper)) {
            selected.push_back(file.file_number);
        }
    }
    return selected;
}

bool TestRelevantSstableRangeSelection() {
    const std::vector<minilsm::FileMetaData> files = {
        RangeFile(1U, "a", "c"),
        RangeFile(2U, "d", "f"),
        RangeFile(3U, "g", "k"),
        RangeFile(4U, "m", "z"),
    };

    MINILSM_CHECK(SelectOverlapping(
        files, std::string("l"), std::string("m")).empty());
    MINILSM_CHECK(SelectOverlapping(
        files, std::string("b"), std::string("c")) ==
        std::vector<std::uint64_t>({1U}));
    MINILSM_CHECK(SelectOverlapping(
        files, std::string("b"), std::string("h")) ==
        std::vector<std::uint64_t>({1U, 2U, 3U}));
    MINILSM_CHECK(SelectOverlapping(
        files, std::string("b"), std::string("e")) ==
        std::vector<std::uint64_t>({1U, 2U}));
    MINILSM_CHECK(SelectOverlapping(
        files, std::nullopt, std::string("d")) ==
        std::vector<std::uint64_t>({1U}));
    MINILSM_CHECK(SelectOverlapping(
        files, std::string("g"), std::nullopt) ==
        std::vector<std::uint64_t>({3U, 4U}));
    MINILSM_CHECK(SelectOverlapping(
        files, std::string("d"), std::string("d")).empty());
    return true;
}

bool TestScanIncludesEveryOverlappingLowerLevelFile() {
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
        directory.path(),
        10U,
        1U,
        {TableValue("a", 100U, "a-l1"),
         TableValue("c", 99U, "c-l1")},
        &file));
    version.levels[1].push_back(file);
    MINILSM_CHECK(BuildTable(
        directory.path(),
        11U,
        1U,
        {TableValue("d", 98U, "d-l1"),
         TableValue("f", 97U, "f-l1")},
        &file));
    version.levels[1].push_back(file);
    MINILSM_CHECK(BuildTable(
        directory.path(),
        12U,
        1U,
        {TableValue("s", 96U, "s-new-l1")},
        &file));
    version.levels[1].push_back(file);

    MINILSM_CHECK(BuildTable(
        directory.path(),
        13U,
        2U,
        {TableValue("b", 90U, "b-l2"),
         TableValue("e", 89U, "e-l2")},
        &file));
    version.levels[2].push_back(file);
    MINILSM_CHECK(BuildTable(
        directory.path(),
        14U,
        2U,
        {TableValue("s", 80U, "s-old-l2")},
        &file));
    version.levels[2].push_back(file);

    MINILSM_CHECK(BuildTable(
        directory.path(),
        15U,
        3U,
        {TableValue("g", 70U, "g-l3"),
         TableValue("z", 69U, "z-l3")},
        &file));
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
    MINILSM_CHECK(ExpectEntries(
        db->Scan(std::string("b"), std::string("t")),
        {{"b", "b-l2"},
         {"c", "c-l1"},
         {"d", "d-l1"},
         {"e", "e-l2"},
         {"f", "f-l1"},
         {"g", "g-l3"},
         {"s", "s-new-l1"}}));
    MINILSM_CHECK(db->Close().ok());
    return true;
}

bool CorruptDataBlockChecksum(
    const std::filesystem::path& path) {
    std::unique_ptr<minilsm::SstableReader> reader;
    MINILSM_CHECK(minilsm::SstableReader::Open(path, &reader).ok());
    MINILSM_CHECK(reader->footer().index_offset > 0U);
    const off_t checksum_byte = static_cast<off_t>(
        reader->footer().index_offset - 1U);

    const int descriptor = ::open(path.c_str(), O_RDWR);
    MINILSM_CHECK(descriptor >= 0);
    unsigned char byte = 0U;
    MINILSM_CHECK(::pread(descriptor, &byte, 1U, checksum_byte) == 1);
    byte ^= 0x01U;
    MINILSM_CHECK(::pwrite(descriptor, &byte, 1U, checksum_byte) == 1);
    MINILSM_CHECK(::close(descriptor) == 0);
    return true;
}

bool TestNonRelevantCorruptionIsNotOpenedAndRelevantErrorPropagates() {
    TemporaryDirectory directory;
    minilsm::Options options = LegacyOptions();
    options.memtable_bytes_limit = 1U;
    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(minilsm::DB::Open(
        options, directory.path(), &db).ok());
    MINILSM_CHECK(db->Put("a", "first").ok());
    MINILSM_CHECK(db->Put("z", "last").ok());

    minilsm::Version version;
    MINILSM_CHECK(ReadActiveVersion(directory.path(), &version));
    std::uint64_t z_file_number = 0U;
    for (const minilsm::FileMetaData& file : version.levels[0]) {
        if (file.smallest_user_key == "z") {
            z_file_number = file.file_number;
        }
    }
    MINILSM_CHECK(z_file_number != 0U);
    MINILSM_CHECK(CorruptDataBlockChecksum(NumberedPath(
        directory.path(), minilsm::FileKind::kSstable, z_file_number)));

    // The corrupted [z,z] file is outside both scans and must not be opened.
    MINILSM_CHECK(ExpectEntries(
        db->Scan(std::string("a"), std::string("b")),
        {{"a", "first"}}));
    MINILSM_CHECK(ExpectEntries(
        db->Scan(std::string("b"), std::string("z")), {}));

    std::unique_ptr<minilsm::Iterator> corrupt =
        db->Scan(std::string("z"), std::nullopt);
    MINILSM_CHECK(corrupt != nullptr);
    MINILSM_CHECK(!corrupt->Valid());
    MINILSM_CHECK(
        corrupt->status().code() ==
        minilsm::Status::Code::kCorruption);
    MINILSM_CHECK(db->Close().ok());
    return true;
}

bool TestOwnedSstableReadersSurviveDatabaseDestruction() {
    TemporaryDirectory directory;
    minilsm::Options options = LegacyOptions();
    options.memtable_bytes_limit = 1U;
    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(minilsm::DB::Open(
        options, directory.path(), &db).ok());
    MINILSM_CHECK(db->Put("a", "one").ok());
    MINILSM_CHECK(db->Put("b", "two").ok());

    std::unique_ptr<minilsm::Iterator> iterator =
        db->Scan(std::nullopt, std::nullopt);
    db.reset();
    MINILSM_CHECK(ExpectEntries(
        std::move(iterator), {{"a", "one"}, {"b", "two"}}));
    return true;
}

std::vector<UserEntry> ExpectedRange(
    const std::map<std::string, std::string>& reference,
    const std::optional<std::string>& lower,
    const std::optional<std::string>& upper) {
    std::vector<UserEntry> expected;
    if (lower.has_value() && upper.has_value() &&
        minilsm::CompareUserKeys(*lower, *upper) >= 0) {
        return expected;
    }
    for (const auto& entry : reference) {
        if (lower.has_value() &&
            minilsm::CompareUserKeys(entry.first, *lower) < 0) {
            continue;
        }
        if (upper.has_value() &&
            minilsm::CompareUserKeys(entry.first, *upper) >= 0) {
            continue;
        }
        expected.push_back(entry);
    }
    return expected;
}

bool TestDeterministicRandomizedScanReferenceModel() {
    constexpr std::uint64_t kSeed = 0x4D365343414E5246ULL;
    std::mt19937_64 random(kSeed);
    TemporaryDirectory directory;
    minilsm::Options options = LegacyOptions();
    options.memtable_bytes_limit = 70U;
    options.l0_compaction_file_trigger =
        minilsm::kMaxManifestLiveFiles;
    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(minilsm::DB::Open(
        options, directory.path(), &db).ok());

    std::vector<std::string> keys;
    for (std::size_t index = 0U; index < 18U; ++index) {
        keys.push_back("key-" + std::to_string(index));
    }
    std::map<std::string, std::string> reference;
    for (std::size_t operation = 0U; operation < 280U; ++operation) {
        const std::string& key =
            keys[static_cast<std::size_t>(random() % keys.size())];
        const std::uint64_t choice = random() % 4U;
        if (choice < 2U) {
            const std::string value =
                "value-" + std::to_string(random() % 100'000U);
            MINILSM_CHECK(db->Put(key, value).ok());
            reference[key] = value;
        } else if (choice == 2U) {
            MINILSM_CHECK(db->Delete(key).ok());
            reference.erase(key);
        } else {
            const minilsm::LookupResult result = db->Get(key);
            const auto expected = reference.find(key);
            if (expected == reference.end()) {
                MINILSM_CHECK(
                    result.status.code() ==
                    minilsm::Status::Code::kNotFound);
            } else {
                MINILSM_CHECK(result.status.ok());
                MINILSM_CHECK(result.value == expected->second);
            }
        }

        if ((operation + 1U) % 37U == 0U) {
            MINILSM_CHECK(db->Close().ok());
            db.reset();
            MINILSM_CHECK(minilsm::DB::Open(
                options, directory.path(), &db).ok());
        }
    }

    MINILSM_CHECK(db->Close().ok());
    db.reset();
    MINILSM_CHECK(minilsm::DB::Open(
        options, directory.path(), &db).ok());

    std::vector<std::optional<std::string>> bounds = {std::nullopt};
    for (const std::string& key : keys) {
        bounds.push_back(key);
    }
    bounds.push_back(std::string("key-05-middle"));
    bounds.push_back(std::string("a"));
    bounds.push_back(std::string("z"));

    for (std::size_t scan = 0U; scan < 100U; ++scan) {
        const std::optional<std::string> lower =
            bounds[static_cast<std::size_t>(random() % bounds.size())];
        const std::optional<std::string> upper =
            bounds[static_cast<std::size_t>(random() % bounds.size())];
        MINILSM_CHECK(ExpectEntries(
            db->Scan(lower, upper),
            ExpectedRange(reference, lower, upper)));
    }
    MINILSM_CHECK(ExpectEntries(
        db->Scan(std::nullopt, std::nullopt),
        ExpectedRange(reference, std::nullopt, std::nullopt)));
    MINILSM_CHECK(db->Close().ok());
    return true;
}

}  // namespace

int main() {
    MINILSM_RUN(TestPublicHalfOpenBoundsAndBinaryKeys);
    MINILSM_RUN(TestVisibilityAcrossMemtableAndManyL0Files);
    MINILSM_RUN(TestRelevantSstableRangeSelection);
    MINILSM_RUN(TestScanIncludesEveryOverlappingLowerLevelFile);
    MINILSM_RUN(
        TestNonRelevantCorruptionIsNotOpenedAndRelevantErrorPropagates);
    MINILSM_RUN(TestOwnedSstableReadersSurviveDatabaseDestruction);
    MINILSM_RUN(TestDeterministicRandomizedScanReferenceModel);
    return 0;
}
