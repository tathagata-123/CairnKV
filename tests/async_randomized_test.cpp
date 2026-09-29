#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iterator>
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

#include <unistd.h>

#include "common/file_io.hpp"
#include "common/file_names.hpp"
#include "common/internal_key.hpp"
#include "db/db_lifecycle.hpp"
#include "manifest/manifest.hpp"
#include "manifest/version.hpp"
#include "minilsm/db.hpp"
#include "minilsm/iterator.hpp"
#include "minilsm/options.hpp"
#include "minilsm/status.hpp"
#include "sstable/sstable_reader.hpp"
#include "test_harness.hpp"

namespace {

constexpr std::size_t kOperationsPerSeed = 800U;
constexpr std::array<std::uint64_t, 2U> kSeeds = {
    0x4D3852414E443031ULL,  // "M8RAND01"
    0x4D3852414E443032ULL,  // "M8RAND02"
};

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern =
            (std::filesystem::temp_directory_path() /
             "minilsm-async-randomized-XXXXXX")
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

struct UserKeyLess {
    [[nodiscard]] bool operator()(
        const std::string& left,
        const std::string& right) const {
        return minilsm::CompareUserKeys(left, right) < 0;
    }
};

using ReferenceMap =
    std::map<std::string, std::string, UserKeyLess>;
using UserKeySet = std::set<std::string, UserKeyLess>;
using UserEntry = std::pair<std::string, std::string>;

struct TestStats {
    std::size_t puts = 0U;
    std::size_t overwrites = 0U;
    std::size_t deletes = 0U;
    std::size_t resurrections = 0U;
    std::size_t gets = 0U;
    std::size_t scans = 0U;
    std::size_t reopens = 0U;
    std::size_t invariant_checks = 0U;
    std::uint64_t mutations = 0U;
    std::array<bool, minilsm::kLevelCount> saw_level = {};
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
    MINILSM_CHECK(size <= std::numeric_limits<std::size_t>::max());
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
    MINILSM_CHECK(ReadWholeFile(
        directory / minilsm::CurrentFileName(false), &current));
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

std::vector<std::string> MakeKeys() {
    std::vector<std::string> keys;
    for (std::size_t index = 0U; index < 48U; ++index) {
        const std::string prefix = index < 10U ? "key-0" : "key-";
        keys.push_back(prefix + std::to_string(index));
    }
    keys.push_back(std::string("\0-key", 5U));
    std::string byte_80(1U, static_cast<char>(0x80U));
    byte_80.append("-key");
    keys.push_back(std::move(byte_80));
    std::string byte_ff(1U, static_cast<char>(0xFFU));
    byte_ff.append("-key");
    keys.push_back(std::move(byte_ff));
    return keys;
}

std::vector<std::optional<std::string>> MakeBounds(
    const std::vector<std::string>& keys) {
    std::vector<std::optional<std::string>> bounds = {
        std::nullopt,
        std::string(),
        std::string("a"),
        std::string("key-00-middle"),
        std::string("key-19-middle"),
        std::string("key-47-middle"),
        std::string("z"),
    };
    for (const std::string& key : keys) {
        bounds.push_back(key);
    }
    std::string byte_7f(1U, static_cast<char>(0x7FU));
    std::string byte_80(1U, static_cast<char>(0x80U));
    std::string byte_ff(1U, static_cast<char>(0xFFU));
    bounds.push_back(std::move(byte_7f));
    bounds.push_back(std::move(byte_80));
    bounds.push_back(std::move(byte_ff));
    return bounds;
}

std::vector<UserEntry> ExpectedRange(
    const ReferenceMap& reference,
    const std::optional<std::string>& lower,
    const std::optional<std::string>& upper) {
    std::vector<UserEntry> expected;
    if (lower.has_value() && upper.has_value() &&
        minilsm::CompareUserKeys(*lower, *upper) >= 0) {
        return expected;
    }
    for (const auto& [key, value] : reference) {
        if (lower.has_value() &&
            minilsm::CompareUserKeys(key, *lower) < 0) {
            continue;
        }
        if (upper.has_value() &&
            minilsm::CompareUserKeys(key, *upper) >= 0) {
            continue;
        }
        expected.emplace_back(key, value);
    }
    return expected;
}

bool ExpectScan(
    const minilsm::DB& db,
    const ReferenceMap& reference,
    const std::optional<std::string>& lower,
    const std::optional<std::string>& upper,
    TestStats* stats) {
    const std::vector<UserEntry> expected =
        ExpectedRange(reference, lower, upper);
    std::unique_ptr<minilsm::Iterator> iterator = db.Scan(lower, upper);
    MINILSM_CHECK(iterator != nullptr);

    std::size_t position = 0U;
    std::optional<std::string> previous;
    while (iterator->Valid()) {
        MINILSM_CHECK(position < expected.size());
        MINILSM_CHECK(iterator->key() == expected[position].first);
        MINILSM_CHECK(iterator->value() == expected[position].second);
        if (previous.has_value()) {
            MINILSM_CHECK(
                minilsm::CompareUserKeys(*previous, iterator->key()) < 0);
        }
        if (lower.has_value()) {
            MINILSM_CHECK(
                minilsm::CompareUserKeys(iterator->key(), *lower) >= 0);
        }
        if (upper.has_value()) {
            MINILSM_CHECK(
                minilsm::CompareUserKeys(iterator->key(), *upper) < 0);
        }
        previous = std::string(iterator->key());
        iterator->Next();
        ++position;
    }
    MINILSM_CHECK(iterator->status().ok());
    MINILSM_CHECK(position == expected.size());
    ++stats->scans;
    return true;
}

bool ExpectGet(
    const minilsm::DB& db,
    const ReferenceMap& reference,
    std::string_view key,
    TestStats* stats) {
    const minilsm::LookupResult actual = db.Get(key);
    const auto expected = reference.find(std::string(key));
    if (expected == reference.end()) {
        MINILSM_CHECK(
            actual.status.code() == minilsm::Status::Code::kNotFound);
        MINILSM_CHECK(!actual.value.has_value());
    } else {
        MINILSM_CHECK(actual.status.ok());
        MINILSM_CHECK(
            actual.value == std::optional<std::string>(expected->second));
    }
    ++stats->gets;
    return true;
}

bool CheckAllGets(
    const minilsm::DB& db,
    const ReferenceMap& reference,
    const std::vector<std::string>& keys,
    TestStats* stats) {
    for (const std::string& key : keys) {
        MINILSM_CHECK(ExpectGet(db, reference, key, stats));
    }
    for (std::size_t index = 0U; index < 6U; ++index) {
        MINILSM_CHECK(ExpectGet(
            db,
            reference,
            "never-written-" + std::to_string(index),
            stats));
    }
    return true;
}

bool CheckManyRanges(
    const minilsm::DB& db,
    const ReferenceMap& reference,
    const std::vector<std::optional<std::string>>& bounds,
    std::mt19937_64* random,
    TestStats* stats) {
    MINILSM_CHECK(ExpectScan(
        db, reference, std::nullopt, std::nullopt, stats));
    MINILSM_CHECK(ExpectScan(
        db, reference, std::string("key-10"), std::string("key-20"),
        stats));
    MINILSM_CHECK(ExpectScan(
        db, reference, std::string("key-20"), std::string("key-10"),
        stats));
    for (std::size_t scan = 0U; scan < 9U; ++scan) {
        const std::optional<std::string>& lower =
            bounds[static_cast<std::size_t>((*random)() % bounds.size())];
        const std::optional<std::string>& upper =
            bounds[static_cast<std::size_t>((*random)() % bounds.size())];
        MINILSM_CHECK(ExpectScan(
            db, reference, lower, upper, stats));
    }
    return true;
}

bool ValidatePersistedState(
    const std::filesystem::path& directory,
    std::uint64_t committed_mutations,
    bool require_all_installed,
    TestStats* stats) {
    minilsm::Version version;
    MINILSM_CHECK(ReadActiveVersion(directory, &version));
    MINILSM_CHECK(minilsm::ValidateVersionMetadata(version).ok());
    MINILSM_CHECK(version.last_sequence <= committed_mutations);
    if (require_all_installed) {
        MINILSM_CHECK(version.last_sequence == committed_mutations);
    }

    std::set<std::uint64_t> live_numbers;
    MINILSM_CHECK(live_numbers.insert(
        version.manifest_file_number).second);
    MINILSM_CHECK(live_numbers.insert(
        version.active_wal_file_number).second);
    std::uint64_t largest_file_number = std::max(
        version.manifest_file_number, version.active_wal_file_number);

    const minilsm::InternalKeyComparator internal_compare;
    std::size_t referenced_sstable_count = 0U;
    for (std::uint32_t level = 0U;
         level < minilsm::kLevelCount;
         ++level) {
        if (!version.levels[level].empty()) {
            stats->saw_level[level] = true;
        }
        if (level > 0U) {
            MINILSM_CHECK(minilsm::ValidateLevelNonOverlap(
                version.levels[level], level).ok());
        }
        const minilsm::FileMetaData* previous = nullptr;
        for (const minilsm::FileMetaData& file : version.levels[level]) {
            MINILSM_CHECK(file.level == level);
            MINILSM_CHECK(file.file_number != 0U);
            MINILSM_CHECK(file.file_number < version.next_file_number);
            MINILSM_CHECK(live_numbers.insert(file.file_number).second);
            MINILSM_CHECK(file.smallest_sequence != 0U);
            MINILSM_CHECK(
                file.smallest_sequence <= file.largest_sequence);
            MINILSM_CHECK(
                file.largest_sequence <= version.last_sequence);
            MINILSM_CHECK(minilsm::CompareUserKeys(
                file.smallest_user_key, file.largest_user_key) <= 0);
            if (previous != nullptr) {
                if (level == 0U) {
                    MINILSM_CHECK(
                        previous->file_number < file.file_number);
                } else {
                    MINILSM_CHECK(minilsm::CompareUserKeys(
                        previous->largest_user_key,
                        file.smallest_user_key) < 0);
                }
            }

            const std::filesystem::path path = NumberedPath(
                directory,
                minilsm::FileKind::kSstable,
                file.file_number);
            bool exists = false;
            MINILSM_CHECK(minilsm::FileExists(path, &exists).ok());
            MINILSM_CHECK(exists);
            std::uint64_t physical_size = 0U;
            MINILSM_CHECK(
                minilsm::GetFileSize(path, &physical_size).ok());
            MINILSM_CHECK(physical_size == file.file_size);

            minilsm::FileMetaData checked = file;
            std::unique_ptr<minilsm::SstableReader> reader;
            MINILSM_CHECK(minilsm::ValidateSstableFile(
                directory, &checked, &reader).ok());
            MINILSM_CHECK(reader != nullptr);
            MINILSM_CHECK(reader->has_bloom_filter());
            MINILSM_CHECK(
                checked.smallest_internal_key.user_key ==
                file.smallest_user_key);
            MINILSM_CHECK(
                checked.largest_internal_key.user_key ==
                file.largest_user_key);
            MINILSM_CHECK(
                checked.smallest_internal_key.sequence_number >=
                file.smallest_sequence);
            MINILSM_CHECK(
                checked.smallest_internal_key.sequence_number <=
                file.largest_sequence);
            MINILSM_CHECK(
                checked.largest_internal_key.sequence_number >=
                file.smallest_sequence);
            MINILSM_CHECK(
                checked.largest_internal_key.sequence_number <=
                file.largest_sequence);
            MINILSM_CHECK(internal_compare.Compare(
                checked.smallest_internal_key,
                checked.largest_internal_key) <= 0);
            std::string encoded_key;
            MINILSM_CHECK(minilsm::EncodeInternalKey(
                checked.smallest_internal_key, &encoded_key).ok());
            MINILSM_CHECK(minilsm::EncodeInternalKey(
                checked.largest_internal_key, &encoded_key).ok());

            if (file.file_number > largest_file_number) {
                largest_file_number = file.file_number;
            }
            previous = &file;
            ++referenced_sstable_count;
        }
    }
    MINILSM_CHECK(version.next_file_number > largest_file_number);

    bool active_wal_exists = false;
    MINILSM_CHECK(minilsm::FileExists(
        NumberedPath(
            directory,
            minilsm::FileKind::kWal,
            version.active_wal_file_number),
        &active_wal_exists).ok());
    MINILSM_CHECK(active_wal_exists);

    std::vector<std::string> names;
    MINILSM_CHECK(minilsm::ListDirectory(directory, &names).ok());
    std::size_t final_wal_count = 0U;
    std::size_t active_wal_count = 0U;
    std::size_t final_sstable_count = 0U;
    for (const std::string& name : names) {
        minilsm::ParsedFileName parsed;
        const minilsm::Status status =
            minilsm::ParseFileName(name, &parsed);
        if (!status.ok()) {
            continue;
        }
        MINILSM_CHECK(!parsed.temporary);
        if (parsed.kind == minilsm::FileKind::kWal) {
            ++final_wal_count;
            if (parsed.file_number == version.active_wal_file_number) {
                ++active_wal_count;
            }
        } else if (parsed.kind == minilsm::FileKind::kSstable) {
            ++final_sstable_count;
            MINILSM_CHECK(live_numbers.contains(parsed.file_number));
        }
    }
    MINILSM_CHECK(final_wal_count == 1U);
    MINILSM_CHECK(active_wal_count == 1U);
    MINILSM_CHECK(final_sstable_count == referenced_sstable_count);
    ++stats->invariant_checks;
    return true;
}

bool ApplyPut(
    minilsm::DB* db,
    ReferenceMap* reference,
    UserKeySet* deleted_keys,
    std::string key,
    std::string value,
    TestStats* stats) {
    const bool existed = reference->contains(key);
    const bool was_deleted = !existed && deleted_keys->contains(key);
    MINILSM_CHECK(db->Put(key, value).ok());
    (*reference)[std::move(key)] = std::move(value);
    ++stats->puts;
    ++stats->mutations;
    if (existed) {
        ++stats->overwrites;
    }
    if (was_deleted) {
        ++stats->resurrections;
    }
    return true;
}

bool ApplyDelete(
    minilsm::DB* db,
    ReferenceMap* reference,
    UserKeySet* deleted_keys,
    const std::string& key,
    TestStats* stats) {
    MINILSM_CHECK(db->Delete(key).ok());
    reference->erase(key);
    deleted_keys->insert(key);
    ++stats->deletes;
    ++stats->mutations;
    return true;
}

std::string RandomValue(
    std::uint64_t seed,
    std::size_t operation,
    std::mt19937_64* random) {
    std::string value =
        "value-" + std::to_string(seed) + "-" +
        std::to_string(operation) + "-" +
        std::to_string((*random)() % 1'000'000U);
    if (((*random)() % 9U) == 0U) {
        value.push_back('\0');
        value.append("binary-tail");
    }
    return value;
}

std::optional<std::string> PickExistingKey(
    const ReferenceMap& reference,
    std::mt19937_64* random) {
    if (reference.empty()) {
        return std::nullopt;
    }
    auto found = reference.begin();
    std::advance(
        found,
        static_cast<std::ptrdiff_t>((*random)() % reference.size()));
    return found->first;
}

std::optional<std::string> PickDeletedKey(
    const ReferenceMap& reference,
    const UserKeySet& deleted_keys,
    std::mt19937_64* random) {
    std::vector<std::string> candidates;
    for (const std::string& key : deleted_keys) {
        if (!reference.contains(key)) {
            candidates.push_back(key);
        }
    }
    if (candidates.empty()) {
        return std::nullopt;
    }
    return candidates[
        static_cast<std::size_t>((*random)() % candidates.size())];
}

bool ReopenAndCheck(
    const minilsm::Options& options,
    const std::filesystem::path& directory,
    const ReferenceMap& reference,
    const std::vector<std::string>& keys,
    std::unique_ptr<minilsm::DB>* db,
    TestStats* stats) {
    MINILSM_CHECK((*db)->Close().ok());
    MINILSM_CHECK(ValidatePersistedState(
        directory, stats->mutations, true, stats));
    db->reset();
    MINILSM_CHECK(
        minilsm::DB::Open(options, directory, db).ok());
    MINILSM_CHECK(CheckAllGets(**db, reference, keys, stats));
    MINILSM_CHECK(ExpectScan(
        **db, reference, std::nullopt, std::nullopt, stats));
    ++stats->reopens;
    return true;
}

bool RunHistory(std::uint64_t seed) {
    std::mt19937_64 operation_random(seed);
    std::mt19937_64 check_random(seed ^ 0xA55A5AA55AA55AA5ULL);
    TemporaryDirectory directory;
    MINILSM_CHECK(!directory.path().empty());

    // Exercise the public defaults: asynchronous maintenance, prefix blocks,
    // bounded table and block caches, and the overlap-aware compaction picker.
    minilsm::Options options;
    options.memtable_bytes_limit = 220U;
    options.sstable_block_target_bytes = 1024U;
    options.l0_compaction_file_trigger = 3U;
    options.l1_max_bytes = 700U;
    options.l2_max_bytes = 1000U;
    options.compaction_output_file_target_bytes = 300U;

    std::unique_ptr<minilsm::DB> db;
    MINILSM_CHECK(
        minilsm::DB::Open(options, directory.path(), &db).ok());
    const std::vector<std::string> keys = MakeKeys();
    const std::vector<std::optional<std::string>> bounds =
        MakeBounds(keys);
    ReferenceMap reference;
    UserKeySet deleted_keys;
    TestStats stats;

    // A deterministic prelude guarantees overwrite, tombstone visibility,
    // and resurrection coverage independently of the random choices.
    MINILSM_CHECK(ApplyPut(
        db.get(), &reference, &deleted_keys, keys[0], "first", &stats));
    MINILSM_CHECK(ApplyPut(
        db.get(), &reference, &deleted_keys, keys[0], "overwrite", &stats));
    MINILSM_CHECK(ApplyDelete(
        db.get(), &reference, &deleted_keys, keys[0], &stats));
    MINILSM_CHECK(ExpectGet(*db, reference, keys[0], &stats));
    MINILSM_CHECK(ApplyPut(
        db.get(), &reference, &deleted_keys, keys[0], "resurrected", &stats));

    for (std::size_t operation = 0U;
         operation < kOperationsPerSeed;
         ++operation) {
        const std::uint64_t choice = operation_random() % 100U;
        const std::string& random_key = keys[
            static_cast<std::size_t>(operation_random() % keys.size())];
        bool reopened = false;

        if (choice < 38U) {
            MINILSM_CHECK(ApplyPut(
                db.get(),
                &reference,
                &deleted_keys,
                random_key,
                RandomValue(seed, operation, &operation_random),
                &stats));
        } else if (choice < 56U) {
            MINILSM_CHECK(ApplyDelete(
                db.get(),
                &reference,
                &deleted_keys,
                random_key,
                &stats));
        } else if (choice < 70U) {
            MINILSM_CHECK(ExpectGet(
                *db, reference, random_key, &stats));
        } else if (choice < 82U) {
            const std::optional<std::string>& lower = bounds[
                static_cast<std::size_t>(
                    operation_random() % bounds.size())];
            const std::optional<std::string>& upper = bounds[
                static_cast<std::size_t>(
                    operation_random() % bounds.size())];
            MINILSM_CHECK(ExpectScan(
                *db, reference, lower, upper, &stats));
        } else if (choice < 90U) {
            std::optional<std::string> key =
                PickExistingKey(reference, &operation_random);
            if (!key.has_value()) {
                key = random_key;
            }
            MINILSM_CHECK(ApplyPut(
                db.get(),
                &reference,
                &deleted_keys,
                *key,
                RandomValue(seed, operation, &operation_random),
                &stats));
        } else if (choice < 96U) {
            const std::optional<std::string> deleted =
                PickDeletedKey(
                    reference, deleted_keys, &operation_random);
            if (deleted.has_value()) {
                MINILSM_CHECK(ApplyPut(
                    db.get(),
                    &reference,
                    &deleted_keys,
                    *deleted,
                    RandomValue(seed, operation, &operation_random),
                    &stats));
            } else {
                MINILSM_CHECK(ApplyDelete(
                    db.get(),
                    &reference,
                    &deleted_keys,
                    random_key,
                    &stats));
            }
        } else {
            MINILSM_CHECK(ReopenAndCheck(
                options,
                directory.path(),
                reference,
                keys,
                &db,
                &stats));
            reopened = true;
        }

        if ((operation + 1U) % 19U == 0U) {
            MINILSM_CHECK(CheckManyRanges(
                *db,
                reference,
                bounds,
                &check_random,
                &stats));
        }
        if ((operation + 1U) % 23U == 0U) {
            MINILSM_CHECK(CheckAllGets(
                *db, reference, keys, &stats));
        }
        if ((operation + 1U) % 29U == 0U) {
            // Raw directory inspection requires a quiescent authority. Close
            // drains asynchronous maintenance before the original file and
            // metadata invariants are checked, then reopen checks all values.
            MINILSM_CHECK(ReopenAndCheck(
                options, directory.path(), reference, keys, &db, &stats));
            reopened = true;
        }
        if (!reopened && (operation + 1U) % 47U == 0U) {
            MINILSM_CHECK(ReopenAndCheck(
                options,
                directory.path(),
                reference,
                keys,
                &db,
                &stats));
        }
    }

    MINILSM_CHECK(CheckAllGets(*db, reference, keys, &stats));
    MINILSM_CHECK(CheckManyRanges(
        *db, reference, bounds, &check_random, &stats));
    MINILSM_CHECK(ReopenAndCheck(
        options,
        directory.path(),
        reference,
        keys,
        &db,
        &stats));
    MINILSM_CHECK(ValidatePersistedState(
        directory.path(), stats.mutations, true, &stats));

    MINILSM_CHECK(stats.puts > 250U);
    MINILSM_CHECK(stats.overwrites > 80U);
    MINILSM_CHECK(stats.deletes > 100U);
    MINILSM_CHECK(stats.resurrections > 30U);
    MINILSM_CHECK(stats.gets > 1'000U);
    MINILSM_CHECK(stats.scans > 400U);
    MINILSM_CHECK(stats.reopens > 25U);
    MINILSM_CHECK(stats.invariant_checks > 40U);
    MINILSM_CHECK(stats.saw_level[0]);
    MINILSM_CHECK(stats.saw_level[1]);
    MINILSM_CHECK(stats.saw_level[2]);
    MINILSM_CHECK(stats.saw_level[3]);
    MINILSM_CHECK(db->Close().ok());
    return true;
}

}  // namespace

int main() {
    for (const std::uint64_t seed : kSeeds) {
        if (!RunHistory(seed)) {
            return 1;
        }
    }
    return 0;
}
