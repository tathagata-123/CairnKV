#include <cstddef>
#include <optional>
#include <string>

#include "minilsm/db.hpp"
#include "minilsm/format_limits.hpp"
#include "minilsm/iterator.hpp"
#include "minilsm/options.hpp"
#include "minilsm/status.hpp"

int main() {
    minilsm::Options options;
    if (options.sstable_block_target_bytes != 4U * 1024U) {
        return 1;
    }

    const minilsm::Status ok = minilsm::Status::Ok();
    if (!ok.ok() || ok.code() != minilsm::Status::Code::kOk) {
        return 2;
    }

    const minilsm::LookupResult missing{
        minilsm::Status::NotFound("smoke-test missing key"),
        std::nullopt,
    };
    if (missing.status.ok() || missing.value.has_value()) {
        return 3;
    }

    minilsm::DB* db_type_check = nullptr;
    minilsm::Iterator* iterator_type_check = nullptr;
    if (db_type_check != nullptr || iterator_type_check != nullptr) {
        return 4;
    }

    return 0;
}
