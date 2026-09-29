#include "common/file_io.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace minilsm {

namespace {

Status PathError(
    std::string_view operation,
    const std::filesystem::path& path,
    int error_number) {
    return Status::IOError(
        std::string(operation) + " '" + path.string() + "': " +
        std::strerror(error_number));
}

Status ClosedFileError(
    std::string_view operation,
    const std::filesystem::path& path) {
    return Status::IOError(
        std::string(operation) + " on closed file '" +
        path.string() + "'");
}

int CloseDescriptor(int descriptor) {
    // Retrying close after EINTR can close an unrelated reused descriptor on
    // some systems. One close call preserves the exactly-once ownership rule.
    return ::close(descriptor);
}

}  // namespace

SequentialFileWriter::SequentialFileWriter(
    int descriptor,
    std::filesystem::path path)
    : descriptor_(descriptor), path_(std::move(path)) {}

SequentialFileWriter::~SequentialFileWriter() {
    if (descriptor_ >= 0) {
        const int descriptor = descriptor_;
        descriptor_ = -1;
        CloseDescriptor(descriptor);
    }
}

Status SequentialFileWriter::CreateNew(
    const std::filesystem::path& path,
    std::unique_ptr<SequentialFileWriter>* writer) {
    if (writer == nullptr) {
        return Status::InvalidArgument(
            "file writer creation received a null output");
    }

    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int descriptor = ::open(path.c_str(), flags, 0644);
    if (descriptor < 0) {
        return PathError("open new file", path, errno);
    }

    writer->reset(new SequentialFileWriter(descriptor, path));
    return Status::Ok();
}

Status SequentialFileWriter::OpenForAppend(
    const std::filesystem::path& path,
    std::unique_ptr<SequentialFileWriter>* writer) {
    if (writer == nullptr) {
        return Status::InvalidArgument(
            "file writer open received a null output");
    }

    int flags = O_WRONLY | O_APPEND;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0) {
        return PathError("open file for append", path, errno);
    }

    writer->reset(new SequentialFileWriter(descriptor, path));
    return Status::Ok();
}

Status SequentialFileWriter::Append(std::string_view bytes) {
    if (descriptor_ < 0) {
        return ClosedFileError("append", path_);
    }

    std::size_t written = 0U;
    while (written < bytes.size()) {
        const std::size_t remaining = bytes.size() - written;
        const std::size_t request =
            std::min(
                remaining,
                static_cast<std::size_t>(SSIZE_MAX));
        const ssize_t result =
            ::write(descriptor_, bytes.data() + written, request);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return PathError("write file", path_, errno);
        }
        if (result == 0) {
            return Status::IOError(
                "write file '" + path_.string() +
                "': write returned zero before completion");
        }
        written += static_cast<std::size_t>(result);
    }
    return Status::Ok();
}

Status SequentialFileWriter::Truncate(std::uint64_t size) {
    if (descriptor_ < 0) {
        return ClosedFileError("truncate", path_);
    }

    const std::uint64_t maximum_offset =
        static_cast<std::uint64_t>(
            std::numeric_limits<off_t>::max());
    if (size > maximum_offset) {
        return Status::InvalidArgument(
            "file truncate size exceeds POSIX off_t");
    }
    if (::ftruncate(descriptor_, static_cast<off_t>(size)) != 0) {
        return PathError("truncate file", path_, errno);
    }
    return Status::Ok();
}

Status SequentialFileWriter::Sync() {
    if (descriptor_ < 0) {
        return ClosedFileError("sync", path_);
    }

    while (::fsync(descriptor_) != 0) {
        if (errno == EINTR) {
            continue;
        }
        return PathError("sync file", path_, errno);
    }
    return Status::Ok();
}

Status SequentialFileWriter::Close() {
    if (descriptor_ < 0) {
        return Status::Ok();
    }

    const int descriptor = descriptor_;
    descriptor_ = -1;
    if (CloseDescriptor(descriptor) != 0) {
        return PathError("close file", path_, errno);
    }
    return Status::Ok();
}

RandomAccessFileReader::RandomAccessFileReader(
    int descriptor,
    std::filesystem::path path)
    : descriptor_(descriptor), path_(std::move(path)) {}

RandomAccessFileReader::~RandomAccessFileReader() {
    if (descriptor_ >= 0) {
        const int descriptor = descriptor_;
        descriptor_ = -1;
        CloseDescriptor(descriptor);
    }
}

Status RandomAccessFileReader::Open(
    const std::filesystem::path& path,
    std::unique_ptr<RandomAccessFileReader>* reader) {
    if (reader == nullptr) {
        return Status::InvalidArgument(
            "file reader creation received a null output");
    }

    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0) {
        return PathError("open file for reading", path, errno);
    }

    reader->reset(new RandomAccessFileReader(descriptor, path));
    return Status::Ok();
}

Status RandomAccessFileReader::Read(
    std::uint64_t offset,
    std::size_t size,
    std::string* result) const {
    if (result == nullptr) {
        return Status::InvalidArgument(
            "file read received a null output");
    }
    if (descriptor_ < 0) {
        return ClosedFileError("read", path_);
    }

    const std::uint64_t maximum_offset =
        static_cast<std::uint64_t>(
            std::numeric_limits<off_t>::max());
    if (offset > maximum_offset ||
        size > maximum_offset - offset) {
        return Status::InvalidArgument(
            "file read offset and size exceed POSIX off_t");
    }
    if (size > result->max_size()) {
        return Status::InvalidArgument(
            "file read size exceeds string capacity");
    }

    std::string bytes(size, '\0');
    std::size_t read = 0U;
    while (read < size) {
        const std::size_t remaining = size - read;
        const std::size_t request =
            std::min(
                remaining,
                static_cast<std::size_t>(SSIZE_MAX));
        const off_t current_offset =
            static_cast<off_t>(offset + read);
        const ssize_t count =
            ::pread(
                descriptor_, bytes.data() + read, request, current_offset);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return PathError("read file", path_, errno);
        }
        if (count == 0) {
            return Status::IOError(
                "read file '" + path_.string() +
                "': unexpected EOF after " + std::to_string(read) +
                " of " + std::to_string(size) + " bytes");
        }
        read += static_cast<std::size_t>(count);
    }

    *result = std::move(bytes);
    return Status::Ok();
}

Status RandomAccessFileReader::Close() {
    if (descriptor_ < 0) {
        return Status::Ok();
    }

    const int descriptor = descriptor_;
    descriptor_ = -1;
    if (CloseDescriptor(descriptor) != 0) {
        return PathError("close file", path_, errno);
    }
    return Status::Ok();
}

Status SyncDirectory(const std::filesystem::path& directory) {
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int descriptor = ::open(directory.c_str(), flags);
    if (descriptor < 0) {
        return PathError("open directory for sync", directory, errno);
    }

    Status status = Status::Ok();
    while (::fsync(descriptor) != 0) {
        if (errno == EINTR) {
            continue;
        }
        if (errno == EINVAL || errno == ENOTSUP) {
            status = Status::NotSupported(
                "directory synchronization is not supported for '" +
                directory.string() + "'");
        } else {
            status = PathError("sync directory", directory, errno);
        }
        break;
    }

    if (CloseDescriptor(descriptor) != 0 && status.ok()) {
        return PathError("close directory", directory, errno);
    }
    return status;
}

Status AtomicRename(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) {
    if (::rename(source.c_str(), destination.c_str()) != 0) {
        const int error_number = errno;
        return Status::IOError(
            "rename '" + source.string() + "' to '" +
            destination.string() + "': " +
            std::strerror(error_number));
    }
    return Status::Ok();
}

Status DeleteFileIfExists(const std::filesystem::path& path) {
    if (::unlink(path.c_str()) == 0) {
        return Status::Ok();
    }
    if (errno == ENOENT) {
        return Status::Ok();
    }
    return PathError("delete file", path, errno);
}

Status FileExists(
    const std::filesystem::path& path,
    bool* exists) {
    if (exists == nullptr) {
        return Status::InvalidArgument(
            "file existence check received a null output");
    }

    struct stat metadata {};
    if (::lstat(path.c_str(), &metadata) == 0) {
        *exists = true;
        return Status::Ok();
    }
    if (errno == ENOENT) {
        *exists = false;
        return Status::Ok();
    }
    return PathError("check file existence", path, errno);
}

Status GetFileSize(
    const std::filesystem::path& path,
    std::uint64_t* size) {
    if (size == nullptr) {
        return Status::InvalidArgument(
            "file-size check received a null output");
    }

    struct stat metadata {};
    if (::stat(path.c_str(), &metadata) != 0) {
        return PathError("stat file", path, errno);
    }
    if (!S_ISREG(metadata.st_mode)) {
        return Status::IOError(
            "stat file '" + path.string() +
            "': path is not a regular file");
    }
    if (metadata.st_size < 0) {
        return Status::IOError(
            "stat file '" + path.string() +
            "': file has a negative size");
    }

    *size = static_cast<std::uint64_t>(metadata.st_size);
    return Status::Ok();
}

Status ListDirectory(
    const std::filesystem::path& directory,
    std::vector<std::string>* names) {
    if (names == nullptr) {
        return Status::InvalidArgument(
            "directory listing received a null output");
    }

    DIR* stream = ::opendir(directory.c_str());
    if (stream == nullptr) {
        return PathError("open directory", directory, errno);
    }

    std::vector<std::string> result;
    int read_error = 0;
    while (true) {
        errno = 0;
        const dirent* entry = ::readdir(stream);
        if (entry == nullptr) {
            read_error = errno;
            break;
        }
        const std::string_view name(entry->d_name);
        if (name != "." && name != "..") {
            result.emplace_back(name);
        }
    }

    const int close_result = ::closedir(stream);
    if (read_error != 0) {
        return PathError("read directory", directory, read_error);
    }
    if (close_result != 0) {
        return PathError("close directory", directory, errno);
    }

    std::sort(result.begin(), result.end());
    *names = std::move(result);
    return Status::Ok();
}

}  // namespace minilsm
