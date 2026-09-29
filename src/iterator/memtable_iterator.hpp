#pragma once

#include "iterator/internal_iterator.hpp"
#include "memtable/memtable.hpp"

namespace minilsm {

// Owns a bounded copy of one memtable so writes, flush and Close cannot
// invalidate an already-created scan. SSTable inputs stay streaming.
class MemTableIterator final : public InternalIterator {
public:
    explicit MemTableIterator(const MemTable::RecordMap* records);
    MemTableIterator(const MemTableIterator&) = delete;
    MemTableIterator& operator=(const MemTableIterator&) = delete;

    Status SeekToFirst() override;
    Status Seek(const InternalKey& target) override;
    void Next() override;

    [[nodiscard]] bool Valid() const override;
    [[nodiscard]] const InternalKey& key() const override;
    [[nodiscard]] std::string_view value() const override;
    [[nodiscard]] const Status& status() const override;

private:
    MemTable::RecordMap owned_records_;
    const MemTable::RecordMap* records_ = nullptr;
    MemTable::RecordMap::const_iterator current_;
    bool valid_ = false;
    Status status_ = Status::Ok();
};

}  // namespace minilsm
