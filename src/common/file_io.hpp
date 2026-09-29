#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "minilsm/status.hpp"

namespace minilsm {

class SequentialFileWriter {
public:
    // Call Close explicitly when its Status matters. The destructor is the
    // final ownership safeguard if a caller exits early.
    ~SequentialFileWriter();

    SequentialFileWriter(const SequentialFileWriter&) = delete;
    SequentialFileWriter& operator=(const SequentialFileWriter&) = delete;

    // O_EXCL is mandatory: creating a temporary or numbered database file
    // never truncates or overwrites an existing live file.
    static Status CreateNew(
        const std::filesystem::path& path,
        std::unique_ptr<SequentialFileWriter>* writer);
    static Status OpenForAppend(
        const std::filesystem::path& path,
        std::unique_ptr<SequentialFileWriter>* writer);

    Status Append(std::string_view bytes);
    Status Truncate(std::uint64_t size);
    Status Sync();
    Status Close();

private:
    SequentialFileWriter(int descriptor, std::filesystem::path path);

    int descriptor_ = -1;
    std::filesystem::path path_;
};

class RandomAccessFileReader {
public:
    // Call Close explicitly when its Status matters. The destructor is the
    // final ownership safeguard if a caller exits early.
    ~RandomAccessFileReader();

    RandomAccessFileReader(const RandomAccessFileReader&) = delete;
    RandomAccessFileReader& operator=(const RandomAccessFileReader&) = delete;

    static Status Open(
        const std::filesystem::path& path,
        std::unique_ptr<RandomAccessFileReader>* reader);

    // Reads exactly size bytes. An EOF before the requested byte count is an
    // error, and result is changed only after a complete successful read.
    Status Read(
        std::uint64_t offset,
        std::size_t size,
        std::string* result) const;
    Status Close();

private:
    RandomAccessFileReader(int descriptor, std::filesystem::path path);

    int descriptor_ = -1;
    std::filesystem::path path_;
};

Status SyncDirectory(const std::filesystem::path& directory);

// POSIX rename is atomic within one filesystem. It replaces an existing
// destination, which is required for the CURRENT.tmp -> CURRENT step.
// Durability still requires SyncDirectory after a successful rename.
Status AtomicRename(
    const std::filesystem::path& source,
    const std::filesystem::path& destination);

// Idempotent for a missing path and uses unlink, so it cannot remove a
// directory recursively.
Status DeleteFileIfExists(const std::filesystem::path& path);

Status FileExists(
    const std::filesystem::path& path,
    bool* exists);
Status GetFileSize(
    const std::filesystem::path& path,
    std::uint64_t* size);
Status ListDirectory(
    const std::filesystem::path& directory,
    std::vector<std::string>* names);

}  // namespace minilsm
