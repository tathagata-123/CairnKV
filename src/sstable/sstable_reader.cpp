#include "sstable/sstable_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "minilsm/format_limits.hpp"
#include "cache/block_cache.hpp"

namespace minilsm {

namespace {

bool InternalKeysEqual(
    const InternalKey& left,
    const InternalKey& right) {
    return left.user_key == right.user_key &&
           left.sequence_number == right.sequence_number &&
           left.record_type == right.record_type;
}

bool SameUserKeyAndSequence(
    const InternalKey& left,
    const InternalKey& right) {
    return left.user_key == right.user_key &&
           left.sequence_number == right.sequence_number;
}

Status ValidateFooterRegions(
    const SstableFooter& footer,
    std::uint64_t file_size) {
    const std::uint64_t footer_offset =
        file_size - kSstableFooterBytes;

    // Every persisted region uses subtraction-first bounds checks so a
    // malicious offset plus size is never evaluated before validation.
    if (footer.index_offset > file_size ||
        footer.index_size > file_size - footer.index_offset) {
        return Status::Corruption(
            "SSTable index region is outside the file");
    }
    if (footer.index_size >
        std::numeric_limits<std::size_t>::max()) {
        return Status::Corruption(
            "SSTable index is larger than addressable memory");
    }
    const bool filter_absent =
        footer.filter_offset == 0U && footer.filter_size == 0U;
    if (filter_absent) {
        if (footer.index_offset > footer_offset ||
            footer.index_size != footer_offset - footer.index_offset) {
            return Status::Corruption(
                "SSTable without a filter has bytes after its index");
        }
        return Status::Ok();
    }

    if (footer.filter_offset > footer_offset ||
        footer.filter_size > footer_offset - footer.filter_offset ||
        footer.filter_size != footer_offset - footer.filter_offset ||
        footer.filter_size > std::numeric_limits<std::size_t>::max()) {
        return Status::Corruption(
            "SSTable filter region is outside the file");
    }
    if (footer.index_offset > footer.filter_offset ||
        footer.index_size != footer.filter_offset - footer.index_offset) {
        return Status::Corruption(
            "SSTable filter does not immediately follow the index");
    }
    return Status::Ok();
}

}  // namespace

SstableReader::SstableReader(
    std::unique_ptr<RandomAccessFileReader> file_reader,
    std::uint64_t file_size,
    SstableFooter footer,
    std::vector<BlockIndexEntry> index_entries,
    std::optional<BloomFilter> bloom_filter)
    : file_reader_(std::move(file_reader)),
      file_size_(file_size),
      footer_(footer),
      index_entries_(std::move(index_entries)),
      bloom_filter_(std::move(bloom_filter)) {}

Status SstableReader::Open(
    const std::filesystem::path& path,
    std::unique_ptr<SstableReader>* reader) {
    if (reader == nullptr) {
        return Status::InvalidArgument(
            "SSTable open received a null output");
    }

    std::uint64_t file_size = 0U;
    Status status = GetFileSize(path, &file_size);
    if (!status.ok()) {
        return status;
    }
    if (file_size < kMinSstableFileBytes ||
        file_size > kMaxDatabaseFileBytes) {
        return Status::Corruption(
            "SSTable file size is outside version 1 bounds");
    }

    std::unique_ptr<RandomAccessFileReader> file_reader;
    status = RandomAccessFileReader::Open(path, &file_reader);
    if (!status.ok()) {
        return status;
    }

    const std::uint64_t footer_offset =
        file_size - kSstableFooterBytes;
    std::string footer_bytes;
    status = file_reader->Read(
        footer_offset, kSstableFooterBytes, &footer_bytes);
    if (!status.ok()) {
        return status;
    }
    SstableFooter footer;
    status = DecodeSstableFooter(footer_bytes, &footer);
    if (!status.ok()) {
        return status;
    }
    status = ValidateFooterRegions(footer, file_size);
    if (!status.ok()) {
        return status;
    }

    std::string index_bytes;
    status = file_reader->Read(
        footer.index_offset,
        static_cast<std::size_t>(footer.index_size),
        &index_bytes);
    if (!status.ok()) {
        return status;
    }
    std::vector<BlockIndexEntry> index_entries;
    status = DecodeIndexBlock(
        index_bytes, footer.index_offset, &index_entries);
    if (!status.ok()) {
        return status;
    }

    std::optional<BloomFilter> bloom_filter;
    if (footer.filter_offset != 0U) {
        std::string filter_bytes;
        status = file_reader->Read(
            footer.filter_offset,
            static_cast<std::size_t>(footer.filter_size),
            &filter_bytes);
        if (!status.ok()) {
            return status;
        }
        BloomFilter decoded_filter;
        status = BloomFilter::Decode(filter_bytes, &decoded_filter);
        if (!status.ok()) {
            return status;
        }
        if (decoded_filter.key_count() == 0U ||
            decoded_filter.key_count() > footer.entry_count) {
            return Status::Corruption(
                "Bloom-filter key count exceeds SSTable entry count");
        }
        bloom_filter = std::move(decoded_filter);
    }

    reader->reset(new SstableReader(
        std::move(file_reader),
        file_size,
        footer,
        std::move(index_entries),
        std::move(bloom_filter)));
    return Status::Ok();
}

bool SstableReader::MayContain(std::string_view user_key) const {
    if (!bloom_filter_.has_value()) {
        return true;
    }
    return bloom_filter_->MayContain(user_key);
}

bool SstableReader::has_bloom_filter() const {
    return bloom_filter_.has_value();
}

std::uint64_t SstableReader::bloom_filter_key_count() const {
    if (!bloom_filter_.has_value()) {
        return 0U;
    }
    return bloom_filter_->key_count();
}

std::size_t SstableReader::FindCandidateBlock(
    const InternalKey& target) const {
    const InternalKeyComparator compare;
    std::size_t first = 0U;
    std::size_t count = index_entries_.size();
    while (count > 0U) {
        const std::size_t step = count / 2U;
        const std::size_t middle = first + step;
        if (compare(index_entries_[middle].largest_internal_key, target)) {
            first = middle + 1U;
            count -= step + 1U;
        } else {
            count = step;
        }
    }
    return first;
}

void SstableReader::SetBlockCache(std::shared_ptr<BlockCache> cache, std::uint64_t file_number) {
    block_cache_ = std::move(cache);
    file_number_ = file_number;
}

Status SstableReader::ReadDataBlock(std::size_t block_index, DataBlockReader* block) const {
    if (!block) return Status::InvalidArgument("null data block output");
    std::shared_ptr<const DataBlockReader> shared;
    Status status = ReadSharedDataBlock(block_index, &shared);
    if (status.ok()) *block = *shared;
    return status;
}

Status SstableReader::ReadSharedDataBlock(
    std::size_t block_index, std::shared_ptr<const DataBlockReader>* block) const {
    if (block == nullptr) {
        return Status::InvalidArgument(
            "SSTable data-block read received a null output");
    }
    if (block_index >= index_entries_.size()) {
        return Status::InvalidArgument(
            "SSTable data-block index is out of range");
    }

    const BlockIndexEntry& handle = index_entries_[block_index];
    if (handle.block_offset > file_size_ ||
        handle.block_size > file_size_ - handle.block_offset ||
        handle.block_offset > footer_.index_offset ||
        handle.block_size > footer_.index_offset - handle.block_offset) {
        return Status::Corruption(
            "SSTable data-block handle is outside the data region");
    }

    if (block_cache_) {
        auto cached = block_cache_->Lookup(file_number_, handle.block_offset);
        if (cached) { *block = std::move(cached); return Status::Ok(); }
    }
    std::string bytes;
    Status status = file_reader_->Read(
        handle.block_offset,
        static_cast<std::size_t>(handle.block_size),
        &bytes);
    if (!status.ok()) {
        return status;
    }
    if (block_cache_) block_cache_->RecordRead(bytes.size());
    auto decoded = std::make_shared<DataBlockReader>();
    status = DataBlockReader::Decode(bytes, decoded.get());
    if (!status.ok()) {
        return status;
    }
    if (decoded->entries().empty() ||
        !InternalKeysEqual(
            decoded->entries().back().internal_key,
            handle.largest_internal_key)) {
        return Status::Corruption(
            "data-block largest key does not match the sparse index");
    }
    if (block_index > 0U) {
        const InternalKey& previous_largest =
            index_entries_[block_index - 1U].largest_internal_key;
        const InternalKey& current_first =
            decoded->entries().front().internal_key;
        const InternalKeyComparator compare;
        if (SameUserKeyAndSequence(previous_largest, current_first) ||
            compare.Compare(previous_largest, current_first) >= 0) {
            return Status::Corruption(
                "data-block boundary keys are not strictly increasing");
        }
    }

    if (block_cache_) block_cache_->Insert(file_number_, handle.block_offset, decoded);
    *block = std::move(decoded);
    return Status::Ok();
}

Status SstableReader::ValidateAndReadProperties(
    SstableProperties* properties) const {
    if (properties == nullptr) {
        return Status::InvalidArgument(
            "SSTable property validation received a null output");
    }

    SstableProperties decoded;
    decoded.file_size = file_size_;
    std::uint64_t distinct_user_key_count = 0U;
    std::optional<std::string> previous_user_key;
    for (std::size_t block_index = 0U;
         block_index < index_entries_.size();
         ++block_index) {
        DataBlockReader block;
        Status status = ReadDataBlock(block_index, &block);
        if (!status.ok()) {
            return status;
        }
        for (const DataBlockEntry& entry : block.entries()) {
            if (!previous_user_key.has_value() ||
                *previous_user_key != entry.internal_key.user_key) {
                ++distinct_user_key_count;
                previous_user_key = entry.internal_key.user_key;
            }
            if (bloom_filter_.has_value() &&
                !bloom_filter_->MayContain(entry.internal_key.user_key)) {
                return Status::Corruption(
                    "Bloom filter rejects a stored user key");
            }
            if (decoded.entry_count == 0U) {
                decoded.smallest_internal_key = entry.internal_key;
                decoded.smallest_sequence =
                    entry.internal_key.sequence_number;
                decoded.largest_sequence =
                    entry.internal_key.sequence_number;
            } else {
                if (entry.internal_key.sequence_number <
                    decoded.smallest_sequence) {
                    decoded.smallest_sequence =
                        entry.internal_key.sequence_number;
                }
                if (entry.internal_key.sequence_number >
                    decoded.largest_sequence) {
                    decoded.largest_sequence =
                        entry.internal_key.sequence_number;
                }
            }
            if (decoded.entry_count ==
                std::numeric_limits<std::uint64_t>::max()) {
                return Status::Corruption(
                    "SSTable entry count overflows uint64");
            }
            ++decoded.entry_count;
            decoded.largest_internal_key = entry.internal_key;
        }
    }
    if (decoded.entry_count == 0U ||
        decoded.entry_count != footer_.entry_count) {
        return Status::Corruption(
            "SSTable footer entry count does not match its data blocks");
    }
    if (bloom_filter_.has_value() &&
        bloom_filter_->key_count() != distinct_user_key_count) {
        return Status::Corruption(
            "Bloom-filter key count does not match stored user keys");
    }

    *properties = std::move(decoded);
    return Status::Ok();
}

LookupResult SstableReader::Get(const InternalKey& target) const {
    std::string encoded_target;
    Status status = EncodeInternalKey(target, &encoded_target);
    if (!status.ok()) {
        return LookupResult{status, std::nullopt};
    }

    const std::size_t block_index = FindCandidateBlock(target);
    if (block_index == index_entries_.size()) {
        return LookupResult{
            Status::NotFound("InternalKey is not present in SSTable"),
            std::nullopt,
        };
    }

    std::shared_ptr<const DataBlockReader> block;
    status = ReadSharedDataBlock(block_index, &block);
    if (!status.ok()) {
        return LookupResult{status, std::nullopt};
    }
    const std::size_t entry_index = block->LowerBound(target);
    if (entry_index == block->entries().size() ||
        !InternalKeysEqual(
            block->entries()[entry_index].internal_key, target)) {
        return LookupResult{
            Status::NotFound("InternalKey is not present in SSTable"),
            std::nullopt,
        };
    }
    return LookupResult{
        Status::Ok(), block->entries()[entry_index].value};
}

const std::vector<BlockIndexEntry>& SstableReader::index() const {
    return index_entries_;
}

const SstableFooter& SstableReader::footer() const {
    return footer_;
}

std::uint64_t SstableReader::entry_count() const {
    return footer_.entry_count;
}

std::uint64_t SstableReader::file_size() const {
    return file_size_;
}

}  // namespace minilsm
