#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
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
#include "db/db_impl.hpp"
#include "manifest/manifest.hpp"
#include "manifest/version.hpp"
#include "minilsm/db.hpp"
#include "minilsm/format_limits.hpp"
#include "minilsm/options.hpp"
#include "minilsm/status.hpp"
#include "test_harness.hpp"
#include "legacy_options.hpp"
#include "wal/wal.hpp"

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern =
            (std::filesystem::temp_directory_path() /
             "minilsm-db-tests-XXXXXX")
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

std::filesystem::path WalPath(
    const std::filesystem::path& directory,
    std::uint64_t file_number = 1U) {
    std::string name;
    const minilsm::Status status =
        minilsm::MakeWalFileName(file_number, false, &name);
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
    MINILSM_CHECK(
        reader->Read(0U, static_cast<std::size_t>(size), bytes).ok());
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
    std::string manifest_name;
    MINILSM_CHECK(
        minilsm::MakeManifestFileName(
            manifest_number, false, &manifest_name)
            .ok());
    std::string manifest;
    MINILSM_CHECK(ReadWholeFile(directory / manifest_name, &manifest));
    MINILSM_CHECK(
        minilsm::DecodeManifestSnapshot(
            manifest, manifest_number, version)
            .ok());
    return true;
}

bool CheckValue(
    const minilsm::DB& db,
    std::string_view key,
    std::string_view expected) {
    const minilsm::LookupResult result = db.Get(key);
    MINILSM_CHECK(result.status.ok());
    MINILSM_CHECK(result.value.has_value());
    MINILSM_CHECK(*result.value == expected);
    return true;
}

bool CheckNotFound(
    const minilsm::DB& db,
    std::string_view key) {
    const minilsm::LookupResult result = db.Get(key);
    MINILSM_CHECK(
        result.status.code() ==
        minilsm::Status::Code::kNotFound);
    MINILSM_CHECK(!result.value.has_value());
    return true;
}

bool TestBasicPublicOperations() {
    TemporaryDirectory parent;
    MINILSM_CHECK(!parent.path().empty());
    const std::filesystem::path database = parent.path() / "database";

    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(
        minilsm::DB::Open(LegacyOptions(), database, &db).ok());
    MINILSM_CHECK(std::filesystem::is_directory(database));
    MINILSM_CHECK(CheckNotFound(*db, "absent"));

    MINILSM_CHECK(db->Put("key", "first").ok());
    MINILSM_CHECK(CheckValue(*db, "key", "first"));
    MINILSM_CHECK(db->Put("key", "second").ok());
    MINILSM_CHECK(CheckValue(*db, "key", "second"));
    MINILSM_CHECK(db->Delete("key").ok());
    MINILSM_CHECK(CheckNotFound(*db, "key"));
    MINILSM_CHECK(db->Delete("never-present").ok());
    MINILSM_CHECK(CheckNotFound(*db, "never-present"));
    MINILSM_CHECK(db->Put("key", "resurrected").ok());
    MINILSM_CHECK(CheckValue(*db, "key", "resurrected"));

    const std::string binary_key("\0\x80key\xFF", 6U);
    const std::string binary_value("\0value\xFF", 7U);
    MINILSM_CHECK(db->Put(binary_key, binary_value).ok());
    MINILSM_CHECK(CheckValue(*db, binary_key, binary_value));
    MINILSM_CHECK(db->Put("", "").ok());
    MINILSM_CHECK(CheckValue(*db, "", ""));
    std::unique_ptr<minilsm::Iterator> iterator =
        db->Scan(std::nullopt, std::nullopt);
    MINILSM_CHECK(iterator != nullptr);
    MINILSM_CHECK(iterator->Valid());
    MINILSM_CHECK(iterator->key().empty());
    MINILSM_CHECK(iterator->value().empty());
    MINILSM_CHECK(iterator->status().ok());

    MINILSM_CHECK(db->Close().ok());
    MINILSM_CHECK(db->Close().ok());
    MINILSM_CHECK(
        db->Put("closed", "value").code() ==
        minilsm::Status::Code::kIOError);
    MINILSM_CHECK(
        db->Get("key").status.code() ==
        minilsm::Status::Code::kIOError);
    return true;
}

bool TestReplayAndNextSequenceRestoration() {
    TemporaryDirectory directory;
    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(
        minilsm::DB::Open(
            LegacyOptions(), directory.path(), &db)
            .ok());
    MINILSM_CHECK(db->Put("alpha", "one").ok());
    MINILSM_CHECK(db->Put("alpha", "two").ok());
    MINILSM_CHECK(db->Delete("beta").ok());
    MINILSM_CHECK(db->Close().ok());

    db.reset();
    MINILSM_CHECK(
        minilsm::DB::Open(
            LegacyOptions(), directory.path(), &db)
            .ok());
    MINILSM_CHECK(CheckValue(*db, "alpha", "two"));
    MINILSM_CHECK(CheckNotFound(*db, "beta"));
    MINILSM_CHECK(db->Put("beta", "restored").ok());
    MINILSM_CHECK(db->Delete("alpha").ok());
    MINILSM_CHECK(db->Close().ok());

    // M4 Close installs committed records in L0 and rotates the active WAL.
    // Preserve the M2 sequence-restoration property by checking the complete
    // authoritative snapshot rather than requiring obsolete WAL-1 to remain.
    minilsm::Version version;
    MINILSM_CHECK(ReadActiveVersion(directory.path(), &version));
    MINILSM_CHECK(version.last_sequence == 5U);
    MINILSM_CHECK(version.levels[0].size() == 2U);
    MINILSM_CHECK(version.active_wal_file_number != 1U);
    bool old_wal_exists = true;
    MINILSM_CHECK(
        minilsm::FileExists(
            WalPath(directory.path(), 1U), &old_wal_exists)
            .ok());
    MINILSM_CHECK(!old_wal_exists);

    db.reset();
    MINILSM_CHECK(
        minilsm::DB::Open(
            LegacyOptions(), directory.path(), &db)
            .ok());
    MINILSM_CHECK(CheckNotFound(*db, "alpha"));
    MINILSM_CHECK(CheckValue(*db, "beta", "restored"));
    MINILSM_CHECK(db->Close().ok());
    return true;
}

bool TestTornTailIsRemovedBeforeNewAppends() {
    TemporaryDirectory directory;
    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(
        minilsm::DB::Open(
            LegacyOptions(), directory.path(), &db)
            .ok());
    // Initialize a valid M4 CURRENT/MANIFEST/active-WAL state, then model a
    // crash that leaves one complete record and an incomplete final frame.
    MINILSM_CHECK(db->Close().ok());
    db.reset();

    const std::filesystem::path wal = WalPath(directory.path());
    std::unique_ptr<minilsm::WalWriter> wal_writer;
    MINILSM_CHECK(
        minilsm::WalWriter::OpenForAppend(
            wal, minilsm::kWalFileHeaderBytes, 0U, &wal_writer)
            .ok());
    MINILSM_CHECK(
        wal_writer
            ->AppendAndSync(
                minilsm::InternalKey{
                    "before", 1U, minilsm::RecordType::kValue},
                "crash")
            .ok());
    MINILSM_CHECK(wal_writer->Close().ok());

    std::unique_ptr<minilsm::SequentialFileWriter> file_writer;
    MINILSM_CHECK(
        minilsm::SequentialFileWriter::OpenForAppend(
            wal, &file_writer)
            .ok());
    MINILSM_CHECK(
        file_writer->Append(std::string("\x19\0", 2U)).ok());
    MINILSM_CHECK(file_writer->Close().ok());

    MINILSM_CHECK(
        minilsm::DB::Open(
            LegacyOptions(), directory.path(), &db)
            .ok());
    MINILSM_CHECK(CheckValue(*db, "before", "crash"));
    MINILSM_CHECK(db->Put("after", "recovery").ok());
    MINILSM_CHECK(db->Close().ok());
    db.reset();

    MINILSM_CHECK(
        minilsm::DB::Open(
            LegacyOptions(), directory.path(), &db)
            .ok());
    MINILSM_CHECK(CheckValue(*db, "before", "crash"));
    MINILSM_CHECK(CheckValue(*db, "after", "recovery"));
    MINILSM_CHECK(db->Close().ok());
    return true;
}

bool TestWalCorruptionFailsOpen() {
    TemporaryDirectory directory;
    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(
        minilsm::DB::Open(
            LegacyOptions(), directory.path(), &db)
            .ok());
    // Keep WAL corruption coverage against the active WAL selected by a
    // valid M4 manifest; Close on a nonempty memtable would rotate it.
    MINILSM_CHECK(db->Close().ok());
    db.reset();

    const std::filesystem::path wal = WalPath(directory.path());
    std::unique_ptr<minilsm::WalWriter> writer;
    MINILSM_CHECK(
        minilsm::WalWriter::OpenForAppend(
            wal, minilsm::kWalFileHeaderBytes, 0U, &writer)
            .ok());
    MINILSM_CHECK(
        writer
            ->AppendAndSync(
                minilsm::InternalKey{
                    "key", 1U, minilsm::RecordType::kValue},
                "value")
            .ok());
    MINILSM_CHECK(writer->Close().ok());
    std::uint64_t size = 0U;
    MINILSM_CHECK(minilsm::GetFileSize(wal, &size).ok());
    MINILSM_CHECK(size > minilsm::kWalFileHeaderBytes + 4U);
    const int descriptor = ::open(wal.c_str(), O_WRONLY);
    MINILSM_CHECK(descriptor >= 0);
    char byte = '\x5A';
    MINILSM_CHECK(
        ::pwrite(
            descriptor,
            &byte,
            1U,
            static_cast<off_t>(size - 5U)) == 1);
    MINILSM_CHECK(::close(descriptor) == 0);

    const minilsm::Status status =
        minilsm::DB::Open(
            LegacyOptions(), directory.path(), &db);
    MINILSM_CHECK(
        status.code() == minilsm::Status::Code::kCorruption);
    MINILSM_CHECK(db == nullptr);
    return true;
}

bool TestAppendAndSyncFailuresStayInvisible() {
    {
        TemporaryDirectory directory;
        std::unique_ptr<minilsm::DBImpl> db;
        MINILSM_CHECK(
            minilsm::DBImpl::Open(
                LegacyOptions(), directory.path(), &db)
                .ok());
        MINILSM_CHECK(db->Put("stable", "value").ok());
        db->FailNextWalAppendForTesting();
        MINILSM_CHECK(
            db->Put("append-failed", "hidden").code() ==
            minilsm::Status::Code::kIOError);
        MINILSM_CHECK(
            db->Get("append-failed").status.code() ==
            minilsm::Status::Code::kNotFound);
        MINILSM_CHECK(
            db->Get("stable").value ==
            std::optional<std::string>("value"));
        MINILSM_CHECK(
            db->Put("writer-poisoned", "hidden").code() ==
            minilsm::Status::Code::kIOError);
        MINILSM_CHECK(db->Close().ok());
        db.reset();
        std::unique_ptr<minilsm::DB> reopened;
        MINILSM_CHECK(
            minilsm::DB::Open(
                LegacyOptions(), directory.path(), &reopened)
                .ok());
        MINILSM_CHECK(CheckValue(*reopened, "stable", "value"));
        MINILSM_CHECK(CheckNotFound(*reopened, "append-failed"));
        MINILSM_CHECK(CheckNotFound(*reopened, "writer-poisoned"));
        MINILSM_CHECK(reopened->Close().ok());
    }
    {
        TemporaryDirectory directory;
        std::unique_ptr<minilsm::DBImpl> db;
        MINILSM_CHECK(
            minilsm::DBImpl::Open(
                LegacyOptions(), directory.path(), &db)
                .ok());
        MINILSM_CHECK(db->Put("stable", "value").ok());
        db->FailNextWalSyncForTesting();
        MINILSM_CHECK(
            db->Delete("stable").code() ==
            minilsm::Status::Code::kIOError);
        const minilsm::LookupResult stable = db->Get("stable");
        MINILSM_CHECK(stable.status.ok());
        MINILSM_CHECK(
            stable.value == std::optional<std::string>("value"));
        MINILSM_CHECK(
            db->Get("other").status.code() ==
            minilsm::Status::Code::kNotFound);
        MINILSM_CHECK(
            db->Put("writer-poisoned", "hidden").code() ==
            minilsm::Status::Code::kIOError);
        MINILSM_CHECK(db->Close().ok());
        db.reset();
        std::unique_ptr<minilsm::DB> reopened;
        MINILSM_CHECK(
            minilsm::DB::Open(
                LegacyOptions(), directory.path(), &reopened)
                .ok());
        MINILSM_CHECK(CheckValue(*reopened, "stable", "value"));
        MINILSM_CHECK(CheckNotFound(*reopened, "writer-poisoned"));
        MINILSM_CHECK(reopened->Close().ok());
    }
    return true;
}

bool TestValidationAndSequenceExhaustion() {
    TemporaryDirectory parent;
    minilsm::Options no_create = LegacyOptions();
    no_create.create_if_missing = false;
    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(
        minilsm::DB::Open(
            no_create, parent.path() / "missing", &db)
            .code() == minilsm::Status::Code::kNotFound);
    MINILSM_CHECK(db == nullptr);
    MINILSM_CHECK(
        minilsm::DB::Open(no_create, parent.path(), &db).code() ==
        minilsm::Status::Code::kNotFound);
    MINILSM_CHECK(
        minilsm::DB::Open(
            LegacyOptions(), parent.path(), nullptr)
            .code() ==
        minilsm::Status::Code::kInvalidArgument);

    MINILSM_CHECK(
        minilsm::DB::Open(
            LegacyOptions(), parent.path(), &db)
            .ok());
    const std::string oversized_key(
        static_cast<std::size_t>(minilsm::kMaxUserKeyBytes) + 1U,
        'k');
    MINILSM_CHECK(
        db->Put(oversized_key, "value").code() ==
        minilsm::Status::Code::kInvalidArgument);
    const std::string oversized_value(
        static_cast<std::size_t>(minilsm::kMaxValueBytes) + 1U,
        'v');
    MINILSM_CHECK(
        db->Put("key", oversized_value).code() ==
        minilsm::Status::Code::kInvalidArgument);
    MINILSM_CHECK(db->Close().ok());

    TemporaryDirectory exhausted_directory;
    MINILSM_CHECK(
        minilsm::DB::Open(
            LegacyOptions(), exhausted_directory.path(), &db)
            .ok());
    MINILSM_CHECK(db->Close().ok());
    db.reset();
    const std::filesystem::path wal = WalPath(exhausted_directory.path());
    std::unique_ptr<minilsm::WalWriter> writer;
    MINILSM_CHECK(
        minilsm::WalWriter::OpenForAppend(
            wal, minilsm::kWalFileHeaderBytes, 0U, &writer)
            .ok());
    MINILSM_CHECK(
        writer
            ->AppendAndSync(
                minilsm::InternalKey{
                    "last",
                    std::numeric_limits<std::uint64_t>::max(),
                    minilsm::RecordType::kValue},
                "visible")
            .ok());
    MINILSM_CHECK(writer->Close().ok());

    MINILSM_CHECK(
        minilsm::DB::Open(
            LegacyOptions(), exhausted_directory.path(), &db)
            .ok());
    MINILSM_CHECK(CheckValue(*db, "last", "visible"));
    MINILSM_CHECK(
        db->Put("overflow", "rejected").code() ==
        minilsm::Status::Code::kIOError);
    MINILSM_CHECK(CheckNotFound(*db, "overflow"));
    MINILSM_CHECK(db->Close().ok());

    TemporaryDirectory multiple_wals;
    std::unique_ptr<minilsm::WalWriter> first_writer;
    std::unique_ptr<minilsm::WalWriter> second_writer;
    MINILSM_CHECK(
        minilsm::WalWriter::CreateNew(
            WalPath(multiple_wals.path()), 1U, &first_writer)
            .ok());
    std::string second_name;
    MINILSM_CHECK(
        minilsm::MakeWalFileName(2U, false, &second_name).ok());
    MINILSM_CHECK(
        minilsm::WalWriter::CreateNew(
            multiple_wals.path() / second_name,
            2U,
            &second_writer)
            .ok());
    MINILSM_CHECK(first_writer->Close().ok());
    MINILSM_CHECK(second_writer->Close().ok());
    db.reset();
    MINILSM_CHECK(
        minilsm::DB::Open(
            LegacyOptions(), multiple_wals.path(), &db)
            .code() == minilsm::Status::Code::kCorruption);
    MINILSM_CHECK(db == nullptr);
    return true;
}

bool TestDeterministicRandomizedReopenModel() {
    constexpr std::uint64_t kSeed = 0x4D3244425245504CULL;
    std::mt19937_64 random(kSeed);
    TemporaryDirectory directory;
    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(
        minilsm::DB::Open(
            LegacyOptions(), directory.path(), &db)
            .ok());

    std::map<std::string, std::string> reference;
    std::vector<std::string> keys;
    for (std::size_t index = 0U; index < 12U; ++index) {
        std::string key = "key-";
        key.push_back(static_cast<char>(index));
        keys.push_back(std::move(key));
    }
    keys.push_back(std::string("\0\xFF", 2U));

    for (std::size_t operation = 0U; operation < 300U;
         ++operation) {
        const std::string& key =
            keys[static_cast<std::size_t>(random() % keys.size())];
        const std::uint64_t choice = random() % 3U;
        if (choice == 0U) {
            std::string value("value\0", 6U);
            value.append(std::to_string(random() % 10000U));
            MINILSM_CHECK(db->Put(key, value).ok());
            reference[key] = value;
        } else if (choice == 1U) {
            MINILSM_CHECK(db->Delete(key).ok());
            reference.erase(key);
        } else {
            const minilsm::LookupResult actual = db->Get(key);
            const auto expected = reference.find(key);
            if (expected == reference.end()) {
                MINILSM_CHECK(
                    actual.status.code() ==
                    minilsm::Status::Code::kNotFound);
                MINILSM_CHECK(!actual.value.has_value());
            } else {
                MINILSM_CHECK(actual.status.ok());
                MINILSM_CHECK(
                    actual.value ==
                    std::optional<std::string>(expected->second));
            }
        }

        if ((operation + 1U) % 31U == 0U) {
            MINILSM_CHECK(db->Close().ok());
            db.reset();
            MINILSM_CHECK(
                minilsm::DB::Open(
                    LegacyOptions(), directory.path(), &db)
                    .ok());
            for (const std::string& known_key : keys) {
                const minilsm::LookupResult actual =
                    db->Get(known_key);
                const auto expected = reference.find(known_key);
                if (expected == reference.end()) {
                    MINILSM_CHECK(
                        actual.status.code() ==
                        minilsm::Status::Code::kNotFound);
                } else {
                    MINILSM_CHECK(
                        actual.value ==
                        std::optional<std::string>(
                            expected->second));
                }
            }
        }
    }

    MINILSM_CHECK(db->Close().ok());
    db.reset();
    MINILSM_CHECK(
        minilsm::DB::Open(
            LegacyOptions(), directory.path(), &db)
            .ok());
    for (const std::string& key : keys) {
        const minilsm::LookupResult actual = db->Get(key);
        const auto expected = reference.find(key);
        if (expected == reference.end()) {
            MINILSM_CHECK(
                actual.status.code() ==
                minilsm::Status::Code::kNotFound);
        } else {
            MINILSM_CHECK(
                actual.value ==
                std::optional<std::string>(expected->second));
        }
    }
    MINILSM_CHECK(db->Close().ok());
    return true;
}

}  // namespace

int main() {
    MINILSM_RUN(TestBasicPublicOperations);
    MINILSM_RUN(TestReplayAndNextSequenceRestoration);
    MINILSM_RUN(TestTornTailIsRemovedBeforeNewAppends);
    MINILSM_RUN(TestWalCorruptionFailsOpen);
    MINILSM_RUN(TestAppendAndSyncFailuresStayInvisible);
    MINILSM_RUN(TestValidationAndSequenceExhaustion);
    MINILSM_RUN(TestDeterministicRandomizedReopenModel);
    return 0;
}
