#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cstdlib>
#include <unistd.h>

#include "common/checksum.hpp"
#include "common/coding.hpp"
#include "common/internal_key.hpp"
#include "minilsm/status.hpp"
#include "sstable/bloom_filter.hpp"
#include "sstable/sstable_builder.hpp"
#include "sstable/sstable_reader.hpp"
#include "test_harness.hpp"

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern =
            (std::filesystem::temp_directory_path() /
             "minilsm-bloom-tests-XXXXXX")
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

void StoreFixed32At(
    std::string* bytes,
    std::size_t offset,
    std::uint32_t value) {
    std::string encoded;
    minilsm::PutFixed32(&encoded, value);
    bytes->replace(offset, encoded.size(), encoded);
}

void RefreshBlockChecksum(std::string* bytes) {
    StoreFixed32At(
        bytes,
        bytes->size() - 4U,
        minilsm::Crc32c(
            std::string_view(*bytes).substr(0U, bytes->size() - 4U)));
}

bool TestEmptyOneManyBinaryAndDuplicates() {
    minilsm::BloomFilter filter;
    MINILSM_CHECK(filter.empty());
    MINILSM_CHECK(filter.MayContain("conservative-empty"));
    MINILSM_CHECK(
        minilsm::BloomFilter::Build({}, &filter).code() ==
        minilsm::Status::Code::kInvalidArgument);
    MINILSM_CHECK(filter.empty());

    const std::string binary("\0\x80\xFF", 3U);
    const std::vector<std::string> one_key = {binary};
    MINILSM_CHECK(
        minilsm::BloomFilter::Build(one_key, &filter).ok());
    MINILSM_CHECK(!filter.empty());
    MINILSM_CHECK(filter.key_count() == 1U);
    MINILSM_CHECK(filter.bit_count() == 64U);
    MINILSM_CHECK(filter.MayContain(binary));

    std::vector<std::string> many_keys;
    for (std::size_t index = 0U; index < 257U; ++index) {
        many_keys.push_back(
            "key-" + std::to_string(index) +
            std::string("\0", 1U));
    }
    many_keys.push_back(binary);
    many_keys.push_back("duplicate");
    many_keys.push_back("duplicate");
    MINILSM_CHECK(
        minilsm::BloomFilter::Build(many_keys, &filter).ok());
    MINILSM_CHECK(filter.key_count() == many_keys.size() - 1U);
    for (const std::string& key : many_keys) {
        MINILSM_CHECK(filter.MayContain(key));
    }
    return true;
}

bool TestDeterministicPersistedRoundTrip() {
    const std::vector<std::string> keys = {
        "alpha", "beta", std::string("\0\xFF", 2U), "omega"};
    minilsm::BloomFilter first;
    minilsm::BloomFilter second;
    MINILSM_CHECK(minilsm::BloomFilter::Build(keys, &first).ok());
    MINILSM_CHECK(minilsm::BloomFilter::Build(keys, &second).ok());

    std::string first_bytes;
    std::string second_bytes;
    MINILSM_CHECK(first.Encode(&first_bytes).ok());
    MINILSM_CHECK(second.Encode(&second_bytes).ok());
    MINILSM_CHECK(first_bytes == second_bytes);
    MINILSM_CHECK(first_bytes.substr(4U, 4U) == "MBF1");

    minilsm::BloomFilter decoded;
    MINILSM_CHECK(
        minilsm::BloomFilter::Decode(first_bytes, &decoded).ok());
    MINILSM_CHECK(decoded.key_count() == keys.size());
    MINILSM_CHECK(decoded.bit_count() == 64U);
    for (const std::string& key : keys) {
        MINILSM_CHECK(decoded.MayContain(key));
    }
    return true;
}

bool TestMalformedMetadataAndCorruptBytes() {
    minilsm::BloomFilter filter;
    MINILSM_CHECK(
        minilsm::BloomFilter::Build({"key"}, &filter).ok());
    std::string valid;
    MINILSM_CHECK(filter.Encode(&valid).ok());

    minilsm::BloomFilter decoded;
    MINILSM_CHECK(
        minilsm::BloomFilter::Decode("", &decoded).code() ==
        minilsm::Status::Code::kCorruption);

    std::string bad_version = valid;
    bad_version[8U] = '\x02';
    bad_version[9U] = '\0';
    RefreshBlockChecksum(&bad_version);
    MINILSM_CHECK(
        minilsm::BloomFilter::Decode(bad_version, &decoded).code() ==
        minilsm::Status::Code::kCorruption);

    std::string bad_bits_per_key = valid;
    StoreFixed32At(&bad_bits_per_key, 12U, 9U);
    RefreshBlockChecksum(&bad_bits_per_key);
    MINILSM_CHECK(
        minilsm::BloomFilter::Decode(
            bad_bits_per_key, &decoded).code() ==
        minilsm::Status::Code::kCorruption);

    std::string bad_bit_count = valid;
    StoreFixed32At(&bad_bit_count, 20U, 63U);
    StoreFixed32At(&bad_bit_count, 24U, 0U);
    RefreshBlockChecksum(&bad_bit_count);
    MINILSM_CHECK(
        minilsm::BloomFilter::Decode(bad_bit_count, &decoded).code() ==
        minilsm::Status::Code::kCorruption);

    std::string bad_checksum = valid;
    bad_checksum.back() ^= 0x01;
    MINILSM_CHECK(
        minilsm::BloomFilter::Decode(bad_checksum, &decoded).code() ==
        minilsm::Status::Code::kCorruption);
    return true;
}

bool TestDuplicateInternalKeyVersionsPersistOneUserKey() {
    TemporaryDirectory directory;
    const std::filesystem::path path = directory.path() / "versions.sst";
    std::unique_ptr<minilsm::SstableBuilder> builder;
    MINILSM_CHECK(
        minilsm::SstableBuilder::Create(path, 1024U, &builder).ok());
    MINILSM_CHECK(builder->Add(
        minilsm::InternalKey{
            "same", 7U, minilsm::RecordType::kValue},
        "new").ok());
    MINILSM_CHECK(builder->Add(
        minilsm::InternalKey{
            "same", 5U, minilsm::RecordType::kTombstone},
        "").ok());
    MINILSM_CHECK(builder->Add(
        minilsm::InternalKey{
            "same", 2U, minilsm::RecordType::kValue},
        "old").ok());
    MINILSM_CHECK(builder->Add(
        minilsm::InternalKey{
            "z", 1U, minilsm::RecordType::kValue},
        "last").ok());
    MINILSM_CHECK(builder->Finish().ok());

    std::unique_ptr<minilsm::SstableReader> reader;
    MINILSM_CHECK(minilsm::SstableReader::Open(path, &reader).ok());
    MINILSM_CHECK(reader->has_bloom_filter());
    MINILSM_CHECK(reader->bloom_filter_key_count() == 2U);
    MINILSM_CHECK(reader->MayContain("same"));
    MINILSM_CHECK(reader->MayContain("z"));
    MINILSM_CHECK(reader->footer().filter_offset ==
                   reader->footer().index_offset +
                       reader->footer().index_size);
    MINILSM_CHECK(reader->footer().filter_size >=
                   minilsm::kMinBloomFilterPhysicalBytes);
    return true;
}

bool TestBroadFalsePositiveRateSanity() {
    constexpr std::uint64_t kSeed = 0x4D35424C4F4F4DULL;
    std::mt19937_64 random(kSeed);
    std::vector<std::string> inserted;
    inserted.reserve(2000U);
    for (std::size_t index = 0U; index < 2000U; ++index) {
        inserted.push_back(
            "present-" + std::to_string(index) + "-" +
            std::to_string(random()));
    }

    minilsm::BloomFilter filter;
    MINILSM_CHECK(
        minilsm::BloomFilter::Build(inserted, &filter).ok());
    for (const std::string& key : inserted) {
        MINILSM_CHECK(filter.MayContain(key));
    }

    std::size_t false_positives = 0U;
    constexpr std::size_t kProbeCount = 10'000U;
    for (std::size_t index = 0U; index < kProbeCount; ++index) {
        const std::string absent =
            "absent-" + std::to_string(index) + "-" +
            std::to_string(random());
        if (filter.MayContain(absent)) {
            ++false_positives;
        }
    }
    // A six-probe, ten-bit filter should be near one percent. Five percent
    // is intentionally broad: this catches a broken filter, not variance.
    MINILSM_CHECK(false_positives < kProbeCount / 20U);
    return true;
}

}  // namespace

int main() {
    MINILSM_RUN(TestEmptyOneManyBinaryAndDuplicates);
    MINILSM_RUN(TestDeterministicPersistedRoundTrip);
    MINILSM_RUN(TestMalformedMetadataAndCorruptBytes);
    MINILSM_RUN(TestDuplicateInternalKeyVersionsPersistOneUserKey);
    MINILSM_RUN(TestBroadFalsePositiveRateSanity);
    return 0;
}
