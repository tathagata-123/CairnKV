#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "common/checksum.hpp"
#include "common/coding.hpp"
#include "common/internal_key.hpp"
#include "manifest/manifest.hpp"
#include "sstable/block.hpp"
#include "sstable/bloom_filter.hpp"
#include "sstable/sstable_format.hpp"
#include "wal/wal_format.hpp"

// This target feeds the same bytes into independent parsers. It deliberately
// does not assert that arbitrary bytes are corrupt: a valid object is allowed.
// The property is clean termination without invalid memory access or UB.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view bytes(reinterpret_cast<const char*>(data), size);
    minilsm::WalRecord record;
    (void)minilsm::DecodeWalFrame(bytes, &record);
    std::vector<minilsm::WalRecord> batch_records;
    (void)minilsm::DecodeWalBatchFrame(bytes, &batch_records);
    (void)minilsm::ValidateWalHeader(bytes, 1U);
    std::uint64_t number = 0U;
    (void)minilsm::DecodeCurrent(bytes, &number);
    minilsm::Version version;
    (void)minilsm::DecodeManifestSnapshot(bytes, 1U, &version);
    minilsm::InternalKey key;
    (void)minilsm::DecodeInternalKey(bytes, &key);
    minilsm::DataBlockReader block;
    if (minilsm::DataBlockReader::Decode(bytes, &block).ok()) {
        (void)block.LowerBound({"customer:000004", 7U, minilsm::RecordType::kValue});
    }
    std::vector<minilsm::BlockIndexEntry> index;
    (void)minilsm::DecodeIndexBlock(bytes, 4096U, &index);
    minilsm::BloomFilter filter;
    if (minilsm::BloomFilter::Decode(bytes, &filter).ok()) {
        (void)filter.MayContain("customer:000004");
    }
    minilsm::SstableFooter footer;
    (void)minilsm::DecodeSstableFooter(bytes, &footer);
    return 0;
}

#ifndef MINILSM_LIBFUZZER
int main() {
    constexpr std::uint64_t seed = 0x66757A7A4C534DULL;
    std::mt19937_64 random(seed);
    std::vector<std::string> corpus = {"", "MDB1", "MIB1", "MBF1", "MLSMST01"};
    std::string encoded;
    const minilsm::InternalKey key{"customer:000001", 1U, minilsm::RecordType::kValue};
    (void)minilsm::EncodeWalFrame(key, "value", &encoded);
    corpus.push_back(encoded);
    minilsm::WriteBatch batch;
    batch.Put("a", "one");
    batch.Delete("b");
    if (!minilsm::EncodeWalBatchFrame(1U, batch, &encoded).ok()) return 1;
    corpus.push_back(encoded);
    (void)minilsm::EncodeWalHeader(1U, &encoded);
    corpus.push_back(encoded);
    (void)minilsm::EncodeCurrent(1U, &encoded);
    corpus.push_back(encoded);
    minilsm::Version version;
    version.manifest_file_number = 1U;
    version.active_wal_file_number = 2U;
    version.next_file_number = 3U;
    if (!minilsm::EncodeManifestSnapshot(version, &encoded).ok()) { std::cerr << "fuzz seed failed at line " << __LINE__ << '\n'; return 1; }
    corpus.push_back(encoded);
    minilsm::DataBlockBuilder builder;
    for (std::uint64_t i = 1U; i <= 40U; ++i) {
        const minilsm::InternalKey entry{"customer:" + std::to_string(100000U + i), i,
                                         minilsm::RecordType::kValue};
        if (!builder.Add(entry, "value").ok()) { std::cerr << "fuzz seed failed at line " << __LINE__ << '\n'; return 1; }
    }
    if (!builder.Finish(&encoded).ok()) { std::cerr << "fuzz seed failed at line " << __LINE__ << '\n'; return 1; }
    corpus.push_back(encoded);
    minilsm::BloomFilter filter;
    if (!minilsm::BloomFilter::Build({"customer:000001", "customer:000002"}, &filter).ok()) { std::cerr << "fuzz seed failed at line " << __LINE__ << '\n'; return 1; }
    if (!filter.Encode(&encoded).ok()) { std::cerr << "fuzz seed failed at line " << __LINE__ << '\n'; return 1; }
    corpus.push_back(encoded);
    if (!minilsm::EncodeIndexBlock({{key, 0U, 4096U}}, &encoded).ok()) { std::cerr << "fuzz seed failed at line " << __LINE__ << '\n'; return 1; }
    corpus.push_back(encoded);
    const minilsm::SstableFooter footer{4145U, 48U, 4096U, 49U, 40U};
    if (!minilsm::EncodeSstableFooter(footer, &encoded).ok()) { std::cerr << "fuzz seed failed at line " << __LINE__ << '\n'; return 1; }
    corpus.push_back(encoded);
    for (const auto& bytes : corpus) {
        LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
        for (std::size_t size = 0U; size < bytes.size(); ++size) {
            LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(bytes.data()), size);
        }
    }
    for (std::size_t iteration = 0U; iteration < 20000U; ++iteration) {
        std::string bytes;
        if (iteration % 2U == 0U) {
            bytes.resize(random() % 1024U);
            for (char& byte : bytes) byte = static_cast<char>(random());
        } else {
            bytes = corpus[random() % corpus.size()];
            if (!bytes.empty()) {
                for (std::size_t flips = 0U; flips < 1U + iteration % 4U; ++flips) {
                    bytes[random() % bytes.size()] ^= static_cast<char>(1U << (random() % 8U));
                }
                // Repair the common trailing CRC in half the mutations so
                // parsing reaches length/restart/order checks past integrity.
                if (iteration % 4U == 1U && bytes.size() >= 4U) {
                    const auto crc = minilsm::Crc32c(std::string_view(bytes).substr(0U, bytes.size() - 4U));
                    for (std::size_t i = 0U; i < 4U; ++i) {
                        bytes[bytes.size() - 4U + i] = static_cast<char>(crc >> (8U * i));
                    }
                }
            }
        }
        LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    }
    std::cout << "20,000 deterministic parser mutations plus valid/truncated seeds passed; seed=" << seed << '\n';
    return 0;
}
#endif
