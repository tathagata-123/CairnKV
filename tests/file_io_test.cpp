#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <cstdlib>
#include <unistd.h>

#include "common/file_io.hpp"
#include "minilsm/status.hpp"
#include "test_harness.hpp"

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern =
            (std::filesystem::temp_directory_path() /
             "minilsm-m1-tests-XXXXXX")
                .string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        if (created != nullptr) {
            path_ = created;
        }
    }

    ~TemporaryDirectory() {
        if (!path_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

bool ReadWholeFile(
    const std::filesystem::path& path,
    std::string* contents) {
    std::uint64_t size = 0U;
    MINILSM_CHECK(minilsm::GetFileSize(path, &size).ok());
    std::unique_ptr<minilsm::RandomAccessFileReader> reader;
    MINILSM_CHECK(
        minilsm::RandomAccessFileReader::Open(path, &reader).ok());
    MINILSM_CHECK(
        reader->Read(
                  0U, static_cast<std::size_t>(size), contents)
            .ok());
    MINILSM_CHECK(reader->Close().ok());
    return true;
}

bool TestNormalWriteReadAndClose() {
    TemporaryDirectory directory;
    MINILSM_CHECK(!directory.path().empty());
    const std::filesystem::path file = directory.path() / "normal";

    std::unique_ptr<minilsm::SequentialFileWriter> writer;
    MINILSM_CHECK(
        minilsm::SequentialFileWriter::CreateNew(file, &writer).ok());
    MINILSM_CHECK(writer->Append("").ok());
    MINILSM_CHECK(writer->Append(std::string_view("a\0", 2U)).ok());
    MINILSM_CHECK(writer->Append("bc").ok());
    MINILSM_CHECK(writer->Sync().ok());
    MINILSM_CHECK(writer->Close().ok());
    MINILSM_CHECK(writer->Close().ok());
    MINILSM_CHECK(
        writer->Append("after close").code() ==
        minilsm::Status::Code::kIOError);
    MINILSM_CHECK(
        writer->Sync().code() == minilsm::Status::Code::kIOError);

    bool exists = false;
    MINILSM_CHECK(minilsm::FileExists(file, &exists).ok());
    MINILSM_CHECK(exists);
    std::uint64_t size = 0U;
    MINILSM_CHECK(minilsm::GetFileSize(file, &size).ok());
    MINILSM_CHECK(size == 4U);

    std::unique_ptr<minilsm::RandomAccessFileReader> reader;
    MINILSM_CHECK(
        minilsm::RandomAccessFileReader::Open(file, &reader).ok());
    std::string contents;
    MINILSM_CHECK(reader->Read(0U, 4U, &contents).ok());
    MINILSM_CHECK(contents == std::string("a\0bc", 4U));
    MINILSM_CHECK(reader->Read(2U, 2U, &contents).ok());
    MINILSM_CHECK(contents == "bc");
    MINILSM_CHECK(reader->Read(4U, 0U, &contents).ok());
    MINILSM_CHECK(contents.empty());
    MINILSM_CHECK(
        reader
            ->Read(
                std::numeric_limits<std::uint64_t>::max(),
                1U,
                &contents)
            .code() == minilsm::Status::Code::kInvalidArgument);
    MINILSM_CHECK(reader->Close().ok());
    MINILSM_CHECK(reader->Close().ok());
    MINILSM_CHECK(
        reader->Read(0U, 1U, &contents).code() ==
        minilsm::Status::Code::kIOError);
    return true;
}

bool TestEmptyAndTruncatedReads() {
    TemporaryDirectory directory;
    MINILSM_CHECK(!directory.path().empty());
    const std::filesystem::path empty = directory.path() / "empty";
    std::unique_ptr<minilsm::SequentialFileWriter> writer;
    MINILSM_CHECK(
        minilsm::SequentialFileWriter::CreateNew(empty, &writer).ok());
    MINILSM_CHECK(writer->Close().ok());

    std::unique_ptr<minilsm::RandomAccessFileReader> reader;
    MINILSM_CHECK(
        minilsm::RandomAccessFileReader::Open(empty, &reader).ok());
    std::string result = "old";
    MINILSM_CHECK(reader->Read(0U, 0U, &result).ok());
    MINILSM_CHECK(result.empty());
    result = "old";
    MINILSM_CHECK(
        reader->Read(0U, 1U, &result).code() ==
        minilsm::Status::Code::kIOError);
    MINILSM_CHECK(result == "old");
    MINILSM_CHECK(reader->Close().ok());

    const std::filesystem::path short_file = directory.path() / "short";
    MINILSM_CHECK(
        minilsm::SequentialFileWriter::CreateNew(short_file, &writer)
            .ok());
    MINILSM_CHECK(writer->Append("abc").ok());
    MINILSM_CHECK(writer->Close().ok());
    MINILSM_CHECK(
        minilsm::RandomAccessFileReader::Open(short_file, &reader).ok());
    result = "old";
    MINILSM_CHECK(
        reader->Read(0U, 4U, &result).code() ==
        minilsm::Status::Code::kIOError);
    MINILSM_CHECK(result == "old");
    MINILSM_CHECK(
        reader->Read(2U, 2U, &result).code() ==
        minilsm::Status::Code::kIOError);
    MINILSM_CHECK(result == "old");
    return true;
}

bool TestExclusiveCreationAndDestructor() {
    TemporaryDirectory directory;
    MINILSM_CHECK(!directory.path().empty());
    const std::filesystem::path file = directory.path() / "live";

    {
        std::unique_ptr<minilsm::SequentialFileWriter> writer;
        MINILSM_CHECK(
            minilsm::SequentialFileWriter::CreateNew(file, &writer)
                .ok());
        MINILSM_CHECK(writer->Append("live contents").ok());
    }

    std::unique_ptr<minilsm::SequentialFileWriter> replacement;
    MINILSM_CHECK(
        minilsm::SequentialFileWriter::CreateNew(file, &replacement)
            .code() == minilsm::Status::Code::kIOError);
    MINILSM_CHECK(replacement == nullptr);

    std::string contents;
    MINILSM_CHECK(ReadWholeFile(file, &contents));
    MINILSM_CHECK(contents == "live contents");
    return true;
}

bool TestOpenForAppendAndTruncate() {
    TemporaryDirectory directory;
    MINILSM_CHECK(!directory.path().empty());
    const std::filesystem::path file =
        directory.path() / "append-and-truncate";

    std::unique_ptr<minilsm::SequentialFileWriter> writer;
    MINILSM_CHECK(
        minilsm::SequentialFileWriter::CreateNew(file, &writer).ok());
    MINILSM_CHECK(writer->Append("abcdef").ok());
    MINILSM_CHECK(writer->Close().ok());

    MINILSM_CHECK(
        minilsm::SequentialFileWriter::OpenForAppend(file, &writer)
            .ok());
    MINILSM_CHECK(writer->Truncate(3U).ok());
    MINILSM_CHECK(writer->Append("XY").ok());
    MINILSM_CHECK(writer->Sync().ok());
    MINILSM_CHECK(
        writer->Truncate(
                  std::numeric_limits<std::uint64_t>::max())
            .code() ==
        minilsm::Status::Code::kInvalidArgument);
    MINILSM_CHECK(writer->Close().ok());

    std::string contents;
    MINILSM_CHECK(ReadWholeFile(file, &contents));
    MINILSM_CHECK(contents == "abcXY");

    std::unique_ptr<minilsm::SequentialFileWriter> missing;
    MINILSM_CHECK(
        minilsm::SequentialFileWriter::OpenForAppend(
            directory.path() / "missing", &missing)
            .code() == minilsm::Status::Code::kIOError);
    MINILSM_CHECK(missing == nullptr);
    return true;
}

bool TestRenameSyncDeleteAndListing() {
    TemporaryDirectory directory;
    MINILSM_CHECK(!directory.path().empty());
    const std::filesystem::path temporary =
        directory.path() / "MANIFEST.tmp";
    const std::filesystem::path final = directory.path() / "MANIFEST";

    std::unique_ptr<minilsm::SequentialFileWriter> writer;
    MINILSM_CHECK(
        minilsm::SequentialFileWriter::CreateNew(temporary, &writer)
            .ok());
    MINILSM_CHECK(writer->Append("snapshot").ok());
    MINILSM_CHECK(writer->Sync().ok());
    MINILSM_CHECK(writer->Close().ok());
    MINILSM_CHECK(minilsm::AtomicRename(temporary, final).ok());
    MINILSM_CHECK(minilsm::SyncDirectory(directory.path()).ok());

    bool exists = true;
    MINILSM_CHECK(minilsm::FileExists(temporary, &exists).ok());
    MINILSM_CHECK(!exists);
    MINILSM_CHECK(minilsm::FileExists(final, &exists).ok());
    MINILSM_CHECK(exists);

    const std::filesystem::path second = directory.path() / "second";
    MINILSM_CHECK(
        minilsm::SequentialFileWriter::CreateNew(second, &writer).ok());
    MINILSM_CHECK(writer->Append("old").ok());
    MINILSM_CHECK(writer->Close().ok());

    std::vector<std::string> names;
    MINILSM_CHECK(
        minilsm::ListDirectory(directory.path(), &names).ok());
    MINILSM_CHECK(
        names == std::vector<std::string>({"MANIFEST", "second"}));

    const std::filesystem::path replacement =
        directory.path() / "replacement";
    MINILSM_CHECK(
        minilsm::SequentialFileWriter::CreateNew(replacement, &writer)
            .ok());
    MINILSM_CHECK(writer->Append("new").ok());
    MINILSM_CHECK(writer->Sync().ok());
    MINILSM_CHECK(writer->Close().ok());
    MINILSM_CHECK(minilsm::AtomicRename(replacement, second).ok());
    std::string contents;
    MINILSM_CHECK(ReadWholeFile(second, &contents));
    MINILSM_CHECK(contents == "new");

    MINILSM_CHECK(minilsm::DeleteFileIfExists(final).ok());
    MINILSM_CHECK(minilsm::DeleteFileIfExists(final).ok());
    MINILSM_CHECK(minilsm::FileExists(final, &exists).ok());
    MINILSM_CHECK(!exists);
    MINILSM_CHECK(
        minilsm::DeleteFileIfExists(directory.path()).code() ==
        minilsm::Status::Code::kIOError);

    MINILSM_CHECK(
        minilsm::AtomicRename(
            directory.path() / "missing", directory.path() / "other")
            .code() == minilsm::Status::Code::kIOError);
    MINILSM_CHECK(
        minilsm::SyncDirectory(second).code() ==
        minilsm::Status::Code::kIOError);
    MINILSM_CHECK(
        minilsm::ListDirectory(
            directory.path() / "missing", &names)
            .code() == minilsm::Status::Code::kIOError);
    return true;
}

}  // namespace

int main() {
    MINILSM_RUN(TestNormalWriteReadAndClose);
    MINILSM_RUN(TestEmptyAndTruncatedReads);
    MINILSM_RUN(TestExclusiveCreationAndDestructor);
    MINILSM_RUN(TestOpenForAppendAndTruncate);
    MINILSM_RUN(TestRenameSyncDeleteAndListing);
    return 0;
}
