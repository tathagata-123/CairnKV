#include "minilsm/db.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "db/db_impl.hpp"

namespace minilsm {

DB::DB(std::unique_ptr<DBImpl> impl)
    : impl_(std::move(impl)) {}

DB::~DB() {
    if (impl_ != nullptr) {
        impl_->Close();
    }
}

Status DB::Open(
    const Options& options,
    const std::filesystem::path& directory,
    std::unique_ptr<DB>* db) {
    if (db == nullptr) {
        return Status::InvalidArgument(
            "DB open received a null output");
    }

    std::unique_ptr<DBImpl> impl;
    Status status = DBImpl::Open(options, directory, &impl);
    if (!status.ok()) {
        return status;
    }
    db->reset(new DB(std::move(impl)));
    return Status::Ok();
}

Status DB::Put(
    std::string_view key,
    std::string_view value) {
    return impl_->Put(key, value);
}

Status DB::Write(const WriteOptions& options, const WriteBatch& batch) {
    return impl_->Write(options, batch);
}

Status DB::Delete(std::string_view key) {
    return impl_->Delete(key);
}

LookupResult DB::Get(std::string_view key) const {
    return impl_->Get(key);
}

ReadCounters DB::read_counters() const {
    return impl_->read_counters();
}

void DB::ResetReadCounters() {
    impl_->ResetReadCounters();
}

CompactionCounters DB::compaction_counters() const {
    return impl_->compaction_counters();
}

void DB::ResetCompactionCounters() {
    impl_->ResetCompactionCounters();
}

std::unique_ptr<Iterator> DB::Scan(
    std::optional<std::string> lower_bound,
    std::optional<std::string> upper_bound) const {
    return impl_->Scan(
        std::move(lower_bound), std::move(upper_bound));
}

Status DB::Close() {
    return impl_->Close();
}

}  // namespace minilsm
