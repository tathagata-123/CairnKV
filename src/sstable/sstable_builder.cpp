#include "sstable/sstable_builder.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "minilsm/format_limits.hpp"

namespace minilsm {

namespace {

bool SameUserKeyAndSequence(
    const InternalKey& left,
    const InternalKey& right) {
    return left.user_key == right.user_key &&
           left.sequence_number == right.sequence_number;
}

Status ValidateAndMeasureEntry(
    const InternalKey& internal_key,
    std::string_view value,
    std::size_t* entry_size) {
    if (value.size() > kMaxValueBytes) {
        return Status::InvalidArgument("SSTable value exceeds 64 MiB");
    }
    if (internal_key.record_type == RecordType::kTombstone &&
        !value.empty()) {
        return Status::InvalidArgument(
            "SSTable tombstone must not contain a value");
    }

    std::string encoded_key;
    Status status = EncodeInternalKey(internal_key, &encoded_key);
    if (!status.ok()) {
        return status;
    }
    constexpr std::size_t kEntryFixedBytes = 8U;
    if (encoded_key.size() >
            std::numeric_limits<std::size_t>::max() -
                kEntryFixedBytes ||
        value.size() >
            std::numeric_limits<std::size_t>::max() -
                kEntryFixedBytes - encoded_key.size()) {
        return Status::InvalidArgument("SSTable entry size overflows");
    }
    *entry_size = kEntryFixedBytes + encoded_key.size() + value.size();
    return Status::Ok();
}

}  // namespace

SstableBuilder::SstableBuilder(
    std::unique_ptr<SequentialFileWriter> file_writer,
    std::size_t target_data_block_size, bool prefix_compression)
    : file_writer_(std::move(file_writer)),
      target_data_block_size_(target_data_block_size),
      prefix_compression_(prefix_compression), data_block_(prefix_compression) {}

Status SstableBuilder::Create(
    const std::filesystem::path& path,
    std::size_t target_data_block_size,
    std::unique_ptr<SstableBuilder>* builder, bool prefix_compression) {
    if (builder == nullptr) {
        return Status::InvalidArgument(
            "SSTable builder creation received a null output");
    }
    if (target_data_block_size < kMinSstableBlockTargetBytes ||
        target_data_block_size > kMaxSstableBlockTargetBytes) {
        return Status::InvalidArgument(
            "SSTable block target is outside configured bounds");
    }

    std::unique_ptr<SequentialFileWriter> file_writer;
    Status status =
        SequentialFileWriter::CreateNew(path, &file_writer);
    if (!status.ok()) {
        return status;
    }
    builder->reset(new SstableBuilder(
        std::move(file_writer), target_data_block_size, prefix_compression));
    return Status::Ok();
}

Status SstableBuilder::Add(
    const InternalKey& internal_key,
    std::string_view value) {
    if (finished_) {
        return Status::IOError("SSTable builder is already finished");
    }
    if (failed_) {
        return Status::IOError(
            "SSTable builder is unusable after an I/O failure");
    }

    std::size_t entry_size = 0U;
    Status status =
        ValidateAndMeasureEntry(internal_key, value, &entry_size);
    if (!status.ok()) {
        return status;
    }

    if (last_key_.has_value()) {
        const InternalKeyComparator compare;
        if (SameUserKeyAndSequence(*last_key_, internal_key)) {
            return Status::InvalidArgument(
                "SSTable keys repeat a user-key sequence");
        }
        if (compare.Compare(*last_key_, internal_key) >= 0) {
            return Status::InvalidArgument(
                "SSTable keys must increase strictly");
        }
    }

    const std::size_t current_size =
        data_block_.current_physical_size();
    const bool current_already_oversized =
        current_size > target_data_block_size_;
    const std::size_t block_increment = entry_size +
        (prefix_compression_ && data_block_.entry_count() % 16U == 0U ? 4U : 0U);
    const bool entry_exceeds_remaining =
        !current_already_oversized &&
        block_increment > target_data_block_size_ - current_size;
    const bool decoded_limit = entry_size > kMaxDataBlockBodyBytes -
        kDataBlockHeaderBytes - data_block_.uncompressed_entry_bytes();
    if (!data_block_.empty() &&
        (current_already_oversized || entry_exceeds_remaining || decoded_limit)) {
        status = FlushDataBlock();
        if (!status.ok()) {
            return status;
        }
    }

    if (entry_count_ == std::numeric_limits<std::uint64_t>::max()) {
        return Status::InvalidArgument(
            "SSTable entry count exceeds uint64");
    }
    status = data_block_.Add(internal_key, value);
    if (!status.ok()) {
        return status;
    }
    if (!last_key_.has_value() ||
        CompareUserKeys(last_key_->user_key, internal_key.user_key) != 0) {
        bloom_user_keys_.push_back(internal_key.user_key);
    }
    ++entry_count_;
    last_key_ = internal_key;
    return Status::Ok();
}

Status SstableBuilder::CheckAppendSize(std::size_t size) const {
    if (file_size_ > kMaxDatabaseFileBytes ||
        size > kMaxDatabaseFileBytes - file_size_) {
        return Status::IOError("SSTable would exceed the 1 TiB limit");
    }
    return Status::Ok();
}

Status SstableBuilder::FlushDataBlock() {
    if (data_block_.empty()) {
        return Status::Ok();
    }

    std::string physical_block;
    Status status = data_block_.Finish(&physical_block);
    if (!status.ok()) {
        return status;
    }
    if (physical_block.size() >
        std::numeric_limits<std::uint32_t>::max()) {
        return Status::InvalidArgument(
            "data-block physical size exceeds uint32");
    }
    status = CheckAppendSize(physical_block.size());
    if (!status.ok()) {
        return status;
    }

    status = file_writer_->Append(physical_block);
    if (!status.ok()) {
        failed_ = true;
        return status;
    }
    index_entries_.push_back(BlockIndexEntry{
        data_block_.last_key(),
        file_size_,
        static_cast<std::uint32_t>(physical_block.size()),
    });
    file_size_ += physical_block.size();
    data_block_ = DataBlockBuilder{prefix_compression_};
    return Status::Ok();
}

Status SstableBuilder::Finish() {
    if (finished_) {
        return Status::IOError("SSTable builder is already finished");
    }
    if (failed_) {
        return Status::IOError(
            "SSTable builder is unusable after an I/O failure");
    }
    if (entry_count_ == 0U) {
        finished_ = true;
        Status close_status = file_writer_->Close();
        if (!close_status.ok()) {
            return close_status;
        }
        return Status::InvalidArgument(
            "version 1 SSTables must contain at least one entry");
    }

    Status status = FlushDataBlock();
    if (!status.ok()) {
        return status;
    }

    const std::uint64_t index_offset = file_size_;
    std::string index_block;
    status = EncodeIndexBlock(index_entries_, &index_block);
    if (!status.ok()) {
        return status;
    }
    status = CheckAppendSize(index_block.size());
    if (!status.ok()) {
        return status;
    }
    status = file_writer_->Append(index_block);
    if (!status.ok()) {
        failed_ = true;
        return status;
    }
    file_size_ += index_block.size();

    // Append exactly one full-file user-key filter after the sparse index.
    const std::uint64_t filter_offset = file_size_;
    BloomFilter bloom_filter;
    status = BloomFilter::Build(bloom_user_keys_, &bloom_filter);
    if (!status.ok()) {
        return status;
    }
    std::string filter_block;
    status = bloom_filter.Encode(&filter_block);
    if (!status.ok()) {
        return status;
    }
    status = CheckAppendSize(filter_block.size());
    if (!status.ok()) {
        return status;
    }
    status = file_writer_->Append(filter_block);
    if (!status.ok()) {
        failed_ = true;
        return status;
    }
    file_size_ += filter_block.size();

    const SstableFooter footer{
        filter_offset,
        static_cast<std::uint64_t>(filter_block.size()),
        index_offset,
        static_cast<std::uint64_t>(index_block.size()),
        entry_count_,
    };
    std::string encoded_footer;
    status = EncodeSstableFooter(footer, &encoded_footer);
    if (!status.ok()) {
        return status;
    }
    status = CheckAppendSize(encoded_footer.size());
    if (!status.ok()) {
        return status;
    }
    status = file_writer_->Append(encoded_footer);
    if (!status.ok()) {
        failed_ = true;
        return status;
    }
    file_size_ += encoded_footer.size();

    status = file_writer_->Sync();
    if (!status.ok()) {
        failed_ = true;
        return status;
    }
    status = file_writer_->Close();
    if (!status.ok()) {
        failed_ = true;
        return status;
    }
    finished_ = true;
    return Status::Ok();
}

}  // namespace minilsm
