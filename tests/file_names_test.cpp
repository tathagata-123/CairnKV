#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "common/file_names.hpp"
#include "minilsm/status.hpp"
#include "test_harness.hpp"

namespace {

using minilsm::FileKind;
using minilsm::ParsedFileName;

bool CheckParsed(
    std::string_view name,
    FileKind expected_kind,
    std::uint64_t expected_number,
    bool expected_temporary) {
    ParsedFileName parsed;
    const minilsm::Status status = minilsm::ParseFileName(name, &parsed);
    MINILSM_CHECK(status.ok());
    MINILSM_CHECK(parsed.kind == expected_kind);
    MINILSM_CHECK(parsed.file_number == expected_number);
    MINILSM_CHECK(parsed.temporary == expected_temporary);
    return true;
}

bool TestCreationAndValidParsing() {
    std::string name;
    MINILSM_CHECK(minilsm::MakeWalFileName(1U, false, &name).ok());
    MINILSM_CHECK(name == "WAL-00000000000000000001.log");
    MINILSM_CHECK(CheckParsed(name, FileKind::kWal, 1U, false));

    MINILSM_CHECK(minilsm::MakeWalFileName(42U, true, &name).ok());
    MINILSM_CHECK(name == "WAL-00000000000000000042.log.tmp");
    MINILSM_CHECK(CheckParsed(name, FileKind::kWal, 42U, true));

    MINILSM_CHECK(
        minilsm::MakeSstableFileName(7U, false, &name).ok());
    MINILSM_CHECK(name == "SST-00000000000000000007.sst");
    MINILSM_CHECK(CheckParsed(name, FileKind::kSstable, 7U, false));

    MINILSM_CHECK(
        minilsm::MakeSstableFileName(7U, true, &name).ok());
    MINILSM_CHECK(name == "SST-00000000000000000007.sst.tmp");
    MINILSM_CHECK(CheckParsed(name, FileKind::kSstable, 7U, true));

    const std::uint64_t maximum =
        std::numeric_limits<std::uint64_t>::max();
    MINILSM_CHECK(
        minilsm::MakeManifestFileName(maximum, false, &name).ok());
    MINILSM_CHECK(name == "MANIFEST-18446744073709551615");
    MINILSM_CHECK(
        CheckParsed(name, FileKind::kManifest, maximum, false));

    MINILSM_CHECK(
        minilsm::MakeManifestFileName(3U, true, &name).ok());
    MINILSM_CHECK(name == "MANIFEST-00000000000000000003.tmp");
    MINILSM_CHECK(CheckParsed(name, FileKind::kManifest, 3U, true));

    MINILSM_CHECK(CheckParsed("CURRENT", FileKind::kCurrent, 0U, false));
    MINILSM_CHECK(
        CheckParsed("CURRENT.tmp", FileKind::kCurrent, 0U, true));
    return true;
}

bool TestMalformedAndOverflowedNames() {
    const std::string invalid_names[] = {
        "",
        "CURRENT\n",
        "CURRENT.tmp.extra",
        "WAL-00000000000000000000.log",
        "WAL-0000000000000000001.log",
        "WAL-000000000000000000001.log",
        "WAL-0000000000000000000x.log",
        "WAL-00000000000000000001.sst",
        "WAL-18446744073709551616.log",
        "SST-00000000000000000001.log",
        "SST-00000000000000000001.sst.bad",
        "MANIFEST-00000000000000000000",
        "MANIFEST-18446744073709551616",
        "MANIFEST-00000000000000000001.bad",
        "dir/WAL-00000000000000000001.log",
    };

    for (const std::string& name : invalid_names) {
        ParsedFileName parsed;
        MINILSM_CHECK(
            minilsm::ParseFileName(name, &parsed).code() ==
            minilsm::Status::Code::kInvalidArgument);
    }

    std::string name = "unchanged";
    MINILSM_CHECK(
        minilsm::MakeWalFileName(0U, false, &name).code() ==
        minilsm::Status::Code::kInvalidArgument);
    MINILSM_CHECK(name == "unchanged");
    MINILSM_CHECK(
        minilsm::MakeSstableFileName(0U, false, &name).code() ==
        minilsm::Status::Code::kInvalidArgument);
    MINILSM_CHECK(
        minilsm::MakeManifestFileName(0U, false, &name).code() ==
        minilsm::Status::Code::kInvalidArgument);
    return true;
}

}  // namespace

int main() {
    MINILSM_RUN(TestCreationAndValidParsing);
    MINILSM_RUN(TestMalformedAndOverflowedNames);
    return 0;
}
