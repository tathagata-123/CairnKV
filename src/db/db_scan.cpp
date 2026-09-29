#include "db/db_impl.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/file_names.hpp"
#include "common/internal_key.hpp"
#include "iterator/forward_range_iterator.hpp"
#include "iterator/internal_iterator.hpp"
#include "iterator/memtable_iterator.hpp"
#include "iterator/merging_iterator.hpp"
#include "iterator/sstable_iterator_adapter.hpp"
#include "iterator/user_iterator.hpp"
#include "manifest/version.hpp"
#include "minilsm/format_limits.hpp"
#include "sstable/sstable_reader.hpp"

namespace minilsm {

namespace {

std::unique_ptr<Iterator> StatusIterator(Status status) {
    return std::make_unique<ForwardRangeIterator>(std::move(status));
}

}  // namespace

std::unique_ptr<Iterator> DBImpl::Scan(
    std::optional<std::string> lower_bound,
    std::optional<std::string> upper_bound) const {
    std::unique_lock lock(mutex_);
    if (closed_) {
        return StatusIterator(Status::IOError("database is closed"));
    }
    if ((lower_bound.has_value() &&
         lower_bound->size() > kMaxUserKeyBytes) ||
        (upper_bound.has_value() &&
         upper_bound->size() > kMaxUserKeyBytes)) {
        return StatusIterator(Status::InvalidArgument(
            "scan bound exceeds the 1 MiB user-key limit"));
    }
    if (lower_bound.has_value() && upper_bound.has_value() &&
        CompareUserKeys(*lower_bound, *upper_bound) >= 0) {
        return StatusIterator(Status::Ok());
    }

    std::vector<std::unique_ptr<InternalIterator>> children;
    if (!memtable_.empty()) {
        children.push_back(std::make_unique<MemTableIterator>(
            &memtable_.records()));
    }

    if (immutable_ && !immutable_->empty()) {
        children.push_back(std::make_unique<MemTableIterator>(&immutable_->records()));
    }
    for (const std::vector<FileMetaData>& level : version_.levels) {
        for (const FileMetaData& file : level) {
            if (!SstableOverlapsUserRange(
                    file, lower_bound, upper_bound)) {
                continue;
            }
            std::shared_ptr<SstableReader> reader;
            Status status = table_cache_.Get(file.file_number, &reader);
            if (!status.ok()) return StatusIterator(status);
            children.push_back(
                std::make_unique<SstableIteratorAdapter>(
                    std::move(reader)));
        }
    }

    lock.unlock();
    auto merging =
        std::make_unique<MergingIterator>(std::move(children));
    Status status = Status::Ok();
    if (lower_bound.has_value()) {
        // UINT64_MAX and the first record type form the first possible
        // InternalKey for lower_bound, so no newest version is skipped.
        const InternalKey target{
            *lower_bound,
            std::numeric_limits<std::uint64_t>::max(),
            RecordType::kTombstone,
        };
        status = merging->Seek(target);
    } else {
        status = merging->SeekToFirst();
    }
    if (!status.ok()) {
        return StatusIterator(status);
    }

    auto user = std::make_unique<UserIterator>(std::move(merging));
    return std::make_unique<ForwardRangeIterator>(
        std::move(user), std::move(upper_bound));
}

}  // namespace minilsm
