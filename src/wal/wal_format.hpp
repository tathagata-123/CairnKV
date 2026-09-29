#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "common/internal_key.hpp"
#include "minilsm/status.hpp"
#include "wal/wal.hpp"

namespace minilsm {

Status EncodeWalHeader(
    std::uint64_t wal_file_number,
    std::string* header);
Status ValidateWalHeader(
    std::string_view header,
    std::uint64_t expected_wal_file_number);
Status EncodeWalFrame(
    const InternalKey& internal_key,
    std::string_view value,
    std::string* frame);
Status DecodeWalFrame(
    std::string_view frame,
    WalRecord* record);

Status EncodeWalBatchFrame(
    std::uint64_t start_sequence, const WriteBatch& batch, std::string* frame);
Status DecodeWalBatchFrame(std::string_view frame, std::vector<WalRecord>* records);

}  // namespace minilsm
