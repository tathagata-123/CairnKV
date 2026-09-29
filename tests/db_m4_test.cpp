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
#include "sstable/sstable_builder.hpp"
#include "sstable/sstable_reader.hpp"
#include "test_harness.hpp"
#include "legacy_options.hpp"
#include "wal/wal.hpp"

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern =
            (std::filesystem::temp_directory_path() /
             "minilsm-m4-tests-XXXXXX")
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

bool WriteNewFile(
    const std::filesystem::path& path,
    std::string_view bytes) {
    std::unique_ptr<minilsm::SequentialFileWriter> writer;
    MINILSM_CHECK(
        minilsm::SequentialFileWriter::CreateNew(path, &writer).ok());
    MINILSM_CHECK(writer->Append(bytes).ok());
    MINILSM_CHECK(writer->Sync().ok());
    MINILSM_CHECK(writer->Close().ok());
    return true;
}

bool ReplaceFile(
    const std::filesystem::path& path,
    std::string_view bytes) {
    MINILSM_CHECK(minilsm::DeleteFileIfExists(path).ok());
    return WriteNewFile(path, bytes);
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

std::filesystem::path NumberedPath(
    const std::filesystem::path& directory,
    minilsm::FileKind kind,
    std::uint64_t number,
    bool temporary = false) {
    std::string name;
    minilsm::Status status = minilsm::Status::InvalidArgument("kind");
    if (kind == minilsm::FileKind::kWal) {
        status = minilsm::MakeWalFileName(number, temporary, &name);
    } else if (kind == minilsm::FileKind::kSstable) {
        status = minilsm::MakeSstableFileName(number, temporary, &name);
    } else if (kind == minilsm::FileKind::kManifest) {
        status = minilsm::MakeManifestFileName(number, temporary, &name);
    }
    if (!status.ok()) {
        return {};
    }
    return directory / name;
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
            directory,
            minilsm::FileKind::kManifest,
            manifest_number),
        &manifest));
    MINILSM_CHECK(
        minilsm::DecodeManifestSnapshot(
            manifest, manifest_number, version)
            .ok());
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

bool CheckNotFound(
    const minilsm::DB& db,
    std::string_view key) {
    const minilsm::LookupResult result = db.Get(key);
    MINILSM_CHECK(
        result.status.code() == minilsm::Status::Code::kNotFound);
    MINILSM_CHECK(!result.value.has_value());
    return true;
}

bool BuildOneRecordSstable(
    const std::filesystem::path& directory,
    std::uint64_t file_number,
    const minilsm::InternalKey& internal_key,
    std::string_view value,
    minilsm::FileMetaData* metadata) {
    const std::filesystem::path path = NumberedPath(
        directory, minilsm::FileKind::kSstable, file_number);
    std::unique_ptr<minilsm::SstableBuilder> builder;
    MINILSM_CHECK(
        minilsm::SstableBuilder::Create(path, 1024U, &builder).ok());
    MINILSM_CHECK(builder->Add(internal_key, value).ok());
    MINILSM_CHECK(builder->Finish().ok());

    std::unique_ptr<minilsm::SstableReader> reader;
    MINILSM_CHECK(minilsm::SstableReader::Open(path, &reader).ok());
    minilsm::SstableProperties properties;
    MINILSM_CHECK(reader->ValidateAndReadProperties(&properties).ok());

    metadata->file_number = file_number;
    metadata->level = 0U;
    metadata->file_size = properties.file_size;
    metadata->entry_count = properties.entry_count;
    metadata->smallest_sequence = properties.smallest_sequence;
    metadata->largest_sequence = properties.largest_sequence;
    metadata->smallest_user_key =
        properties.smallest_internal_key.user_key;
    metadata->largest_user_key =
        properties.largest_internal_key.user_key;
    metadata->smallest_internal_key =
        properties.smallest_internal_key;
    metadata->largest_internal_key =
        properties.largest_internal_key;
    return true;
}

bool PreparePendingFlush(
    const std::filesystem::path& directory,
    minilsm::Version* old_version,
    minilsm::Version* new_version) {
    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(
        minilsm::DB::Open(LegacyOptions(), directory, &db).ok());
    MINILSM_CHECK(db->Close().ok());
    db.reset();
    MINILSM_CHECK(ReadActiveVersion(directory, old_version));
    MINILSM_CHECK(old_version->next_file_number == 3U);

    const minilsm::InternalKey key{
        "key", 1U, minilsm::RecordType::kValue};
    std::unique_ptr<minilsm::WalWriter> old_wal_writer;
    MINILSM_CHECK(
        minilsm::WalWriter::OpenForAppend(
            NumberedPath(
                directory,
                minilsm::FileKind::kWal,
                old_version->active_wal_file_number),
            minilsm::kWalFileHeaderBytes,
            0U,
            &old_wal_writer)
            .ok());
    MINILSM_CHECK(old_wal_writer->AppendAndSync(key, "value").ok());
    MINILSM_CHECK(old_wal_writer->Close().ok());

    minilsm::FileMetaData file;
    MINILSM_CHECK(BuildOneRecordSstable(
        directory, 3U, key, "value", &file));
    std::unique_ptr<minilsm::WalWriter> new_wal_writer;
    MINILSM_CHECK(
        minilsm::WalWriter::CreateNew(
            NumberedPath(directory, minilsm::FileKind::kWal, 4U),
            4U,
            &new_wal_writer)
            .ok());
    MINILSM_CHECK(new_wal_writer->Close().ok());

    *new_version = *old_version;
    new_version->manifest_file_number = 5U;
    new_version->active_wal_file_number = 4U;
    new_version->last_sequence = 1U;
    new_version->next_file_number = 6U;
    new_version->levels[0].push_back(file);
    MINILSM_CHECK(
        minilsm::WriteManifestSnapshotFile(
            directory, *new_version, false).ok());
    return true;
}

bool TestInitialCloseFlushAndMinimumSstable() {
    TemporaryDirectory directory;
    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(
        minilsm::DB::Open(
            LegacyOptions(), directory.path(), &db)
            .ok());

    minilsm::Version initial;
    MINILSM_CHECK(ReadActiveVersion(directory.path(), &initial));
    MINILSM_CHECK(initial.levels[0].empty());
    MINILSM_CHECK(initial.active_wal_file_number == 1U);
    MINILSM_CHECK(initial.manifest_file_number == 2U);
    MINILSM_CHECK(initial.next_file_number == 3U);

    MINILSM_CHECK(db->Put("", "").ok());
    MINILSM_CHECK(db->Close().ok());
    db.reset();

    minilsm::Version flushed;
    MINILSM_CHECK(ReadActiveVersion(directory.path(), &flushed));
    MINILSM_CHECK(flushed.levels[0].size() == 1U);
    // M5 adds the minimum 48-byte filter block to the 154-byte compatible
    // version-1 table that M4 produced.
    MINILSM_CHECK(flushed.levels[0][0].file_size == 202U);
    MINILSM_CHECK(flushed.last_sequence == 1U);
    bool obsolete_wal_exists = true;
    MINILSM_CHECK(minilsm::FileExists(
        NumberedPath(directory.path(), minilsm::FileKind::kWal, 1U),
        &obsolete_wal_exists).ok());
    MINILSM_CHECK(!obsolete_wal_exists);

    MINILSM_CHECK(
        minilsm::DB::Open(
            LegacyOptions(), directory.path(), &db)
            .ok());
    MINILSM_CHECK(CheckValue(*db, "", ""));
    MINILSM_CHECK(db->Close().ok());
    return true;
}

bool TestThresholdMultipleL0AndFreshness() {
    TemporaryDirectory directory;
    minilsm::Options options = LegacyOptions();
    options.memtable_bytes_limit = 1U;
    options.l0_compaction_file_trigger =
        minilsm::kMaxManifestLiveFiles;
    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(
        minilsm::DB::Open(options, directory.path(), &db).ok());

    MINILSM_CHECK(db->Put("key", "old").ok());
    MINILSM_CHECK(db->Put("other", "present").ok());
    MINILSM_CHECK(db->Put("key", "new").ok());
    MINILSM_CHECK(CheckValue(*db, "key", "new"));
    MINILSM_CHECK(db->Delete("key").ok());
    MINILSM_CHECK(CheckNotFound(*db, "key"));
    MINILSM_CHECK(db->Put("key", "resurrected").ok());
    MINILSM_CHECK(CheckValue(*db, "key", "resurrected"));

    minilsm::Version version;
    MINILSM_CHECK(ReadActiveVersion(directory.path(), &version));
    MINILSM_CHECK(version.levels[0].size() == 5U);
    for (std::size_t index = 1U; index < version.levels[0].size(); ++index) {
        MINILSM_CHECK(
            version.levels[0][index - 1U].file_number <
            version.levels[0][index].file_number);
    }
    MINILSM_CHECK(db->Close().ok());
    db.reset();

    MINILSM_CHECK(
        minilsm::DB::Open(options, directory.path(), &db).ok());
    MINILSM_CHECK(CheckValue(*db, "key", "resurrected"));
    MINILSM_CHECK(CheckValue(*db, "other", "present"));
    MINILSM_CHECK(db->Close().ok());
    return true;
}

bool TestCurrentAndMissingMetadataFailures() {
    {
        TemporaryDirectory directory;
        std::unique_ptr<minilsm::WalWriter> writer;
        MINILSM_CHECK(minilsm::WalWriter::CreateNew(
            NumberedPath(directory.path(), minilsm::FileKind::kWal, 1U),
            1U,
            &writer).ok());
        MINILSM_CHECK(writer->Close().ok());
        std::unique_ptr<minilsm::DB> db;
        MINILSM_CHECK(
            minilsm::DB::Open(
                LegacyOptions(), directory.path(), &db)
                .code() == minilsm::Status::Code::kCorruption);
    }
    {
        TemporaryDirectory directory;
        std::unique_ptr<minilsm::DB> db;
        MINILSM_CHECK(
            minilsm::DB::Open(
                LegacyOptions(), directory.path(), &db)
                .ok());
        MINILSM_CHECK(db->Close().ok());
        db.reset();
        MINILSM_CHECK(
            minilsm::DeleteFileIfExists(directory.path() / "CURRENT")
                .ok());
        MINILSM_CHECK(
            minilsm::DB::Open(
                LegacyOptions(), directory.path(), &db)
                .code() == minilsm::Status::Code::kCorruption);
    }
    {
        TemporaryDirectory directory;
        std::unique_ptr<minilsm::DB> db;
        MINILSM_CHECK(
            minilsm::DB::Open(
                LegacyOptions(), directory.path(), &db)
                .ok());
        MINILSM_CHECK(db->Close().ok());
        db.reset();
        MINILSM_CHECK(ReplaceFile(
            directory.path() / "CURRENT", "not-a-manifest\n"));
        MINILSM_CHECK(
            minilsm::DB::Open(
                LegacyOptions(), directory.path(), &db)
                .code() == minilsm::Status::Code::kCorruption);
    }
    {
        TemporaryDirectory directory;
        std::unique_ptr<minilsm::DB> db;
        MINILSM_CHECK(
            minilsm::DB::Open(
                LegacyOptions(), directory.path(), &db)
                .ok());
        MINILSM_CHECK(db->Close().ok());
        db.reset();
        std::string missing_manifest;
        MINILSM_CHECK(minilsm::EncodeCurrent(999U, &missing_manifest).ok());
        MINILSM_CHECK(ReplaceFile(
            directory.path() / "CURRENT", missing_manifest));
        MINILSM_CHECK(
            minilsm::DB::Open(
                LegacyOptions(), directory.path(), &db)
                .code() == minilsm::Status::Code::kCorruption);
    }
    return true;
}

bool TestActiveManifestWalAndRangeCorruptionFailOpen() {
    {
        TemporaryDirectory directory;
        std::unique_ptr<minilsm::DB> db;
        MINILSM_CHECK(
            minilsm::DB::Open(
                LegacyOptions(), directory.path(), &db)
                .ok());
        MINILSM_CHECK(db->Close().ok());
        db.reset();
        minilsm::Version version;
        MINILSM_CHECK(ReadActiveVersion(directory.path(), &version));
        MINILSM_CHECK(minilsm::DeleteFileIfExists(NumberedPath(
            directory.path(),
            minilsm::FileKind::kWal,
            version.active_wal_file_number)).ok());
        MINILSM_CHECK(
            minilsm::DB::Open(
                LegacyOptions(), directory.path(), &db)
                .code() == minilsm::Status::Code::kCorruption);
    }
    {
        TemporaryDirectory directory;
        std::unique_ptr<minilsm::DB> db;
        MINILSM_CHECK(
            minilsm::DB::Open(
                LegacyOptions(), directory.path(), &db)
                .ok());
        MINILSM_CHECK(db->Put("key", "value").ok());
        MINILSM_CHECK(db->Close().ok());
        db.reset();
        minilsm::Version version;
        MINILSM_CHECK(ReadActiveVersion(directory.path(), &version));
        version.levels[0][0].smallest_user_key = "jey";
        std::string manifest;
        MINILSM_CHECK(
            minilsm::EncodeManifestSnapshot(version, &manifest).ok());
        MINILSM_CHECK(ReplaceFile(
            NumberedPath(
                directory.path(),
                minilsm::FileKind::kManifest,
                version.manifest_file_number),
            manifest));
        MINILSM_CHECK(
            minilsm::DB::Open(
                LegacyOptions(), directory.path(), &db)
                .code() == minilsm::Status::Code::kCorruption);
    }
    return true;
}

bool TestMissingReferencedSstableFailsOpen() {
    TemporaryDirectory directory;
    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(
        minilsm::DB::Open(
            LegacyOptions(), directory.path(), &db)
            .ok());
    MINILSM_CHECK(db->Put("key", "value").ok());
    MINILSM_CHECK(db->Close().ok());
    db.reset();

    minilsm::Version version;
    MINILSM_CHECK(ReadActiveVersion(directory.path(), &version));
    MINILSM_CHECK(version.levels[0].size() == 1U);
    MINILSM_CHECK(minilsm::DeleteFileIfExists(NumberedPath(
        directory.path(),
        minilsm::FileKind::kSstable,
        version.levels[0][0].file_number)).ok());
    MINILSM_CHECK(
        minilsm::DB::Open(
            LegacyOptions(), directory.path(), &db)
            .code() == minilsm::Status::Code::kCorruption);
    return true;
}

bool TestOrphanCleanupPreservesLiveAndUnknownFiles() {
    TemporaryDirectory directory;
    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(
        minilsm::DB::Open(
            LegacyOptions(), directory.path(), &db)
            .ok());
    MINILSM_CHECK(db->Put("live", "value").ok());
    MINILSM_CHECK(db->Close().ok());
    db.reset();

    minilsm::Version live;
    MINILSM_CHECK(ReadActiveVersion(directory.path(), &live));
    const std::filesystem::path orphans[] = {
        NumberedPath(
            directory.path(), minilsm::FileKind::kSstable, 90U, true),
        NumberedPath(
            directory.path(), minilsm::FileKind::kSstable, 91U),
        NumberedPath(directory.path(), minilsm::FileKind::kWal, 92U),
        NumberedPath(
            directory.path(), minilsm::FileKind::kManifest, 93U, true),
        NumberedPath(
            directory.path(), minilsm::FileKind::kManifest, 94U),
        directory.path() / "CURRENT.tmp",
    };
    for (const std::filesystem::path& orphan : orphans) {
        MINILSM_CHECK(WriteNewFile(orphan, "orphan"));
    }
    const std::filesystem::path unknown = directory.path() / "notes.txt";
    MINILSM_CHECK(WriteNewFile(unknown, "keep"));

    MINILSM_CHECK(
        minilsm::DB::Open(
            LegacyOptions(), directory.path(), &db)
            .ok());
    MINILSM_CHECK(CheckValue(*db, "live", "value"));
    for (const std::filesystem::path& orphan : orphans) {
        bool exists = true;
        MINILSM_CHECK(minilsm::FileExists(orphan, &exists).ok());
        MINILSM_CHECK(!exists);
    }
    bool exists = false;
    MINILSM_CHECK(minilsm::FileExists(unknown, &exists).ok());
    MINILSM_CHECK(exists);
    MINILSM_CHECK(minilsm::FileExists(
        directory.path() / "CURRENT", &exists).ok());
    MINILSM_CHECK(exists);
    MINILSM_CHECK(minilsm::FileExists(NumberedPath(
        directory.path(),
        minilsm::FileKind::kManifest,
        live.manifest_file_number), &exists).ok());
    MINILSM_CHECK(exists);
    MINILSM_CHECK(minilsm::FileExists(NumberedPath(
        directory.path(),
        minilsm::FileKind::kWal,
        live.active_wal_file_number), &exists).ok());
    MINILSM_CHECK(exists);
    MINILSM_CHECK(minilsm::FileExists(NumberedPath(
        directory.path(),
        minilsm::FileKind::kSstable,
        live.levels[0][0].file_number), &exists).ok());
    MINILSM_CHECK(exists);
    MINILSM_CHECK(db->Close().ok());
    return true;
}

bool TestCleanupFailureLeavesAuthoritativeFilesIntact() {
    TemporaryDirectory directory;
    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(
        minilsm::DB::Open(
            LegacyOptions(), directory.path(), &db)
            .ok());
    MINILSM_CHECK(db->Put("live", "value").ok());
    MINILSM_CHECK(db->Close().ok());
    db.reset();

    minilsm::Version live;
    MINILSM_CHECK(ReadActiveVersion(directory.path(), &live));
    const std::filesystem::path undeletable_temporary = NumberedPath(
        directory.path(), minilsm::FileKind::kSstable, 90U, true);
    std::error_code error;
    MINILSM_CHECK(
        std::filesystem::create_directory(
            undeletable_temporary, error));
    MINILSM_CHECK(!error);

    MINILSM_CHECK(
        minilsm::DB::Open(
            LegacyOptions(), directory.path(), &db)
            .code() == minilsm::Status::Code::kIOError);
    MINILSM_CHECK(db == nullptr);

    bool exists = false;
    MINILSM_CHECK(minilsm::FileExists(
        directory.path() / "CURRENT", &exists).ok());
    MINILSM_CHECK(exists);
    MINILSM_CHECK(minilsm::FileExists(NumberedPath(
        directory.path(),
        minilsm::FileKind::kManifest,
        live.manifest_file_number), &exists).ok());
    MINILSM_CHECK(exists);
    MINILSM_CHECK(minilsm::FileExists(NumberedPath(
        directory.path(),
        minilsm::FileKind::kWal,
        live.active_wal_file_number), &exists).ok());
    MINILSM_CHECK(exists);
    MINILSM_CHECK(minilsm::FileExists(NumberedPath(
        directory.path(),
        minilsm::FileKind::kSstable,
        live.levels[0][0].file_number), &exists).ok());
    MINILSM_CHECK(exists);

    MINILSM_CHECK(
        std::filesystem::remove(undeletable_temporary, error));
    MINILSM_CHECK(!error);
    MINILSM_CHECK(
        minilsm::DB::Open(
            LegacyOptions(), directory.path(), &db)
            .ok());
    MINILSM_CHECK(CheckValue(*db, "live", "value"));
    MINILSM_CHECK(db->Close().ok());
    return true;
}

bool TestCrashBeforeCurrentUsesOldWal() {
    TemporaryDirectory directory;
    minilsm::Version old_version;
    minilsm::Version pending_version;
    MINILSM_CHECK(PreparePendingFlush(
        directory.path(), &old_version, &pending_version));

    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(
        minilsm::DB::Open(
            LegacyOptions(), directory.path(), &db)
            .ok());
    MINILSM_CHECK(CheckValue(*db, "key", "value"));
    bool exists = true;
    MINILSM_CHECK(minilsm::FileExists(NumberedPath(
        directory.path(), minilsm::FileKind::kSstable, 3U), &exists).ok());
    MINILSM_CHECK(!exists);
    MINILSM_CHECK(minilsm::FileExists(NumberedPath(
        directory.path(), minilsm::FileKind::kWal, 4U), &exists).ok());
    MINILSM_CHECK(!exists);
    MINILSM_CHECK(minilsm::FileExists(NumberedPath(
        directory.path(), minilsm::FileKind::kManifest, 5U), &exists).ok());
    MINILSM_CHECK(!exists);
    MINILSM_CHECK(db->Close().ok());
    return true;
}

bool TestSstableFinalizationCrashBoundariesUseOldWal() {
    for (std::size_t boundary = 0U; boundary < 2U; ++boundary) {
        TemporaryDirectory directory;
        minilsm::Version old_version;
        minilsm::Version pending_version;
        MINILSM_CHECK(PreparePendingFlush(
            directory.path(), &old_version, &pending_version));
        MINILSM_CHECK(minilsm::DeleteFileIfExists(NumberedPath(
            directory.path(), minilsm::FileKind::kWal, 4U)).ok());
        MINILSM_CHECK(minilsm::DeleteFileIfExists(NumberedPath(
            directory.path(), minilsm::FileKind::kManifest, 5U)).ok());

        const std::filesystem::path final_sstable = NumberedPath(
            directory.path(), minilsm::FileKind::kSstable, 3U);
        std::filesystem::path orphan = final_sstable;
        if (boundary == 0U) {
            MINILSM_CHECK(
                minilsm::DeleteFileIfExists(final_sstable).ok());
            orphan = NumberedPath(
                directory.path(),
                minilsm::FileKind::kSstable,
                3U,
                true);
            MINILSM_CHECK(WriteNewFile(orphan, "partial"));
        }

        std::unique_ptr<minilsm::DB> db;
        MINILSM_CHECK(
            minilsm::DB::Open(
                LegacyOptions(), directory.path(), &db)
                .ok());
        MINILSM_CHECK(CheckValue(*db, "key", "value"));
        bool exists = true;
        MINILSM_CHECK(minilsm::FileExists(orphan, &exists).ok());
        MINILSM_CHECK(!exists);
        MINILSM_CHECK(db->Close().ok());
    }
    return true;
}

bool TestPreCommitFlushFailuresKeepOldStateAuthoritative() {
    for (std::size_t failure = 0U; failure < 5U; ++failure) {
        TemporaryDirectory directory;
        std::unique_ptr<minilsm::DBImpl> db;
        MINILSM_CHECK(
            minilsm::DBImpl::Open(
                LegacyOptions(), directory.path(), &db)
                .ok());
        MINILSM_CHECK(db->Put("key", "value").ok());
        if (failure == 0U) {
            db->FailNextSstableBuildForTesting();
        } else if (failure == 1U) {
            db->FailNextNewWalCreationForTesting();
        } else if (failure == 2U) {
            db->FailNextManifestWriteForTesting();
        } else if (failure == 3U) {
            db->FailNextManifestRenameForTesting();
        } else {
            db->FailNextCurrentInstallForTesting();
        }
        MINILSM_CHECK(
            db->FlushForTesting().code() ==
            minilsm::Status::Code::kIOError);
        MINILSM_CHECK(
            db->Get("key").value ==
            std::optional<std::string>("value"));

        // DBImpl destruction models a process stop without its normal Close
        // flush. The unchanged CURRENT must recover from old WAL-1.
        db.reset();
        std::unique_ptr<minilsm::DB> reopened;
        MINILSM_CHECK(
            minilsm::DB::Open(
                LegacyOptions(), directory.path(), &reopened)
                .ok());
        MINILSM_CHECK(CheckValue(*reopened, "key", "value"));
        minilsm::Version version;
        MINILSM_CHECK(ReadActiveVersion(directory.path(), &version));
        MINILSM_CHECK(version.manifest_file_number == 2U);
        MINILSM_CHECK(version.active_wal_file_number == 1U);
        MINILSM_CHECK(reopened->Close().ok());
    }
    return true;
}

bool TestCrashAfterCurrentUsesNewStateAndIgnoresOldWal() {
    TemporaryDirectory directory;
    minilsm::Version old_version;
    minilsm::Version new_version;
    MINILSM_CHECK(PreparePendingFlush(
        directory.path(), &old_version, &new_version));
    bool current_replaced = false;
    MINILSM_CHECK(minilsm::InstallCurrentFile(
        directory.path(),
        new_version.manifest_file_number,
        &current_replaced,
        false).ok());
    MINILSM_CHECK(current_replaced);

    bool old_wal_exists = false;
    const std::filesystem::path old_wal = NumberedPath(
        directory.path(),
        minilsm::FileKind::kWal,
        old_version.active_wal_file_number);
    MINILSM_CHECK(minilsm::FileExists(old_wal, &old_wal_exists).ok());
    MINILSM_CHECK(old_wal_exists);

    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(
        minilsm::DB::Open(
            LegacyOptions(), directory.path(), &db)
            .ok());
    MINILSM_CHECK(CheckValue(*db, "key", "value"));
    MINILSM_CHECK(minilsm::FileExists(old_wal, &old_wal_exists).ok());
    MINILSM_CHECK(!old_wal_exists);
    MINILSM_CHECK(db->Close().ok());
    return true;
}

bool TestDeterministicRandomizedFlushReopenModel() {
    constexpr std::uint64_t kSeed = 0x4D34464C555348ULL;
    std::mt19937_64 random(kSeed);
    TemporaryDirectory directory;
    minilsm::Options options = LegacyOptions();
    options.memtable_bytes_limit = 80U;
    options.l0_compaction_file_trigger =
        minilsm::kMaxManifestLiveFiles;
    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(
        minilsm::DB::Open(options, directory.path(), &db).ok());

    std::vector<std::string> keys;
    for (std::size_t index = 0U; index < 10U; ++index) {
        keys.push_back("key-" + std::to_string(index));
    }
    std::map<std::string, std::string> reference;
    for (std::size_t operation = 0U; operation < 220U; ++operation) {
        const std::string& key =
            keys[static_cast<std::size_t>(random() % keys.size())];
        const std::uint64_t choice = random() % 3U;
        if (choice == 0U) {
            const std::string value =
                "value-" + std::to_string(random() % 10000U);
            MINILSM_CHECK(db->Put(key, value).ok());
            reference[key] = value;
        } else if (choice == 1U) {
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

        if ((operation + 1U) % 29U == 0U) {
            MINILSM_CHECK(db->Close().ok());
            db.reset();
            MINILSM_CHECK(
                minilsm::DB::Open(options, directory.path(), &db).ok());
            for (const std::string& known_key : keys) {
                const auto expected = reference.find(known_key);
                if (expected == reference.end()) {
                    MINILSM_CHECK(CheckNotFound(*db, known_key));
                } else {
                    MINILSM_CHECK(
                        CheckValue(*db, known_key, expected->second));
                }
            }
        }
    }
    MINILSM_CHECK(db->Close().ok());
    db.reset();
    MINILSM_CHECK(
        minilsm::DB::Open(options, directory.path(), &db).ok());
    for (const std::string& key : keys) {
        const auto expected = reference.find(key);
        if (expected == reference.end()) {
            MINILSM_CHECK(CheckNotFound(*db, key));
        } else {
            MINILSM_CHECK(CheckValue(*db, key, expected->second));
        }
    }
    MINILSM_CHECK(db->Close().ok());
    return true;
}

}  // namespace

int main() {
    MINILSM_RUN(TestInitialCloseFlushAndMinimumSstable);
    MINILSM_RUN(TestThresholdMultipleL0AndFreshness);
    MINILSM_RUN(TestCurrentAndMissingMetadataFailures);
    MINILSM_RUN(TestActiveManifestWalAndRangeCorruptionFailOpen);
    MINILSM_RUN(TestMissingReferencedSstableFailsOpen);
    MINILSM_RUN(TestOrphanCleanupPreservesLiveAndUnknownFiles);
    MINILSM_RUN(TestCleanupFailureLeavesAuthoritativeFilesIntact);
    MINILSM_RUN(TestCrashBeforeCurrentUsesOldWal);
    MINILSM_RUN(TestSstableFinalizationCrashBoundariesUseOldWal);
    MINILSM_RUN(TestPreCommitFlushFailuresKeepOldStateAuthoritative);
    MINILSM_RUN(TestCrashAfterCurrentUsesNewStateAndIgnoresOldWal);
    MINILSM_RUN(TestDeterministicRandomizedFlushReopenModel);
    return 0;
}
