#include <cstddef>
#include <string>

#include "minilsm/format_limits.hpp"
#include "minilsm/options.hpp"
#include "minilsm/status.hpp"
#include "test_harness.hpp"

namespace {

bool TestStatusCodesAndMessages() {
    const minilsm::Status ok = minilsm::Status::Ok();
    MINILSM_CHECK(ok.ok());
    MINILSM_CHECK(ok.code() == minilsm::Status::Code::kOk);
    MINILSM_CHECK(ok.message().empty());

    const minilsm::Status statuses[] = {
        minilsm::Status::NotFound("missing key"),
        minilsm::Status::InvalidArgument("bad argument"),
        minilsm::Status::IOError("write failed"),
        minilsm::Status::Corruption("bad bytes"),
        minilsm::Status::NotSupported("unsupported operation"),
    };
    const minilsm::Status::Code expected_codes[] = {
        minilsm::Status::Code::kNotFound,
        minilsm::Status::Code::kInvalidArgument,
        minilsm::Status::Code::kIOError,
        minilsm::Status::Code::kCorruption,
        minilsm::Status::Code::kNotSupported,
    };

    for (std::size_t index = 0; index < std::size(statuses); ++index) {
        MINILSM_CHECK(!statuses[index].ok());
        MINILSM_CHECK(statuses[index].code() == expected_codes[index]);
        MINILSM_CHECK(!statuses[index].message().empty());
    }
    return true;
}

bool TestOptionsDefaultsAndBounds() {
    minilsm::Options options;
    MINILSM_CHECK(options.create_if_missing);
    MINILSM_CHECK(options.sstable_block_target_bytes == 4U * 1024U);
    MINILSM_CHECK(
        options.memtable_bytes_limit ==
        minilsm::kDefaultMemtableBytesLimit);
    MINILSM_CHECK(
        options.l0_compaction_file_trigger ==
        minilsm::kDefaultL0CompactionFileTrigger);
    MINILSM_CHECK(options.l1_max_bytes == minilsm::kDefaultL1MaxBytes);
    MINILSM_CHECK(options.l2_max_bytes == minilsm::kDefaultL2MaxBytes);
    MINILSM_CHECK(
        options.compaction_output_file_target_bytes ==
        minilsm::kDefaultCompactionOutputFileTargetBytes);
    MINILSM_CHECK(options.Validate().ok());

    options.sstable_block_target_bytes =
        minilsm::kMinSstableBlockTargetBytes;
    MINILSM_CHECK(options.Validate().ok());

    options.sstable_block_target_bytes =
        minilsm::kMaxSstableBlockTargetBytes;
    MINILSM_CHECK(options.Validate().ok());

    options.sstable_block_target_bytes =
        minilsm::kMinSstableBlockTargetBytes - 1U;
    const minilsm::Status too_small = options.Validate();
    MINILSM_CHECK(
        too_small.code() == minilsm::Status::Code::kInvalidArgument);
    MINILSM_CHECK(
        too_small.message().find("sstable_block_target_bytes") !=
        std::string::npos);

    options.sstable_block_target_bytes =
        minilsm::kMaxSstableBlockTargetBytes + 1U;
    MINILSM_CHECK(
        options.Validate().code() ==
        minilsm::Status::Code::kInvalidArgument);

    options.sstable_block_target_bytes =
        minilsm::kDefaultSstableBlockTargetBytes;
    options.memtable_bytes_limit = 0U;
    MINILSM_CHECK(
        options.Validate().code() ==
        minilsm::Status::Code::kInvalidArgument);
    options.memtable_bytes_limit = 1U;
    MINILSM_CHECK(options.Validate().ok());

    options.l0_compaction_file_trigger = 0U;
    MINILSM_CHECK(
        options.Validate().code() ==
        minilsm::Status::Code::kInvalidArgument);
    options.l0_compaction_file_trigger = 1U;
    options.l1_max_bytes = 0U;
    MINILSM_CHECK(
        options.Validate().code() ==
        minilsm::Status::Code::kInvalidArgument);
    options.l1_max_bytes = 1U;
    options.l2_max_bytes = 0U;
    MINILSM_CHECK(
        options.Validate().code() ==
        minilsm::Status::Code::kInvalidArgument);
    options.l2_max_bytes = 1U;
    options.compaction_output_file_target_bytes = 0U;
    MINILSM_CHECK(
        options.Validate().code() ==
        minilsm::Status::Code::kInvalidArgument);
    options.compaction_output_file_target_bytes = 1U;
    MINILSM_CHECK(options.Validate().ok());
    return true;
}

}  // namespace

int main() {
    MINILSM_RUN(TestStatusCodesAndMessages);
    MINILSM_RUN(TestOptionsDefaultsAndBounds);
    return 0;
}
