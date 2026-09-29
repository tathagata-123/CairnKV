#include "iterator/sstable_iterator_adapter.hpp"

#include <memory>
#include <string_view>
#include <utility>

namespace minilsm {

SstableIteratorAdapter::SstableIteratorAdapter(
    std::shared_ptr<SstableReader> reader)
    : reader_(std::move(reader)), iterator_(reader_.get()) {}

Status SstableIteratorAdapter::SeekToFirst() {
    return iterator_.SeekToFirst();
}

Status SstableIteratorAdapter::Seek(const InternalKey& target) {
    return iterator_.Seek(target);
}

void SstableIteratorAdapter::Next() {
    iterator_.Next();
}

bool SstableIteratorAdapter::Valid() const {
    return iterator_.Valid();
}

const InternalKey& SstableIteratorAdapter::key() const {
    return iterator_.key();
}

std::string_view SstableIteratorAdapter::value() const {
    return iterator_.value();
}

const Status& SstableIteratorAdapter::status() const {
    return iterator_.status();
}

}  // namespace minilsm
