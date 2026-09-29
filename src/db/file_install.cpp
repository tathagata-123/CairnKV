#include "db/db_lifecycle.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "common/crash_point.hpp"
#include "common/file_io.hpp"
#include "common/file_names.hpp"
#include "manifest/manifest.hpp"

namespace minilsm {

Status WriteManifestSnapshotFile(
    const std::filesystem::path& directory,
    const Version& version,
    bool fail_before_rename_for_testing) {
    std::string encoded;
    Status status = EncodeManifestSnapshot(version, &encoded);
    if (!status.ok()) {
        return status;
    }

    std::string temporary_name;
    std::string final_name;
    status = MakeManifestFileName(
        version.manifest_file_number, true, &temporary_name);
    if (status.ok()) {
        status = MakeManifestFileName(
            version.manifest_file_number, false, &final_name);
    }
    if (!status.ok()) {
        return status;
    }

    bool final_exists = false;
    status = FileExists(directory / final_name, &final_exists);
    if (!status.ok()) {
        return status;
    }
    if (final_exists) {
        return Status::IOError(
            "refusing to overwrite existing MANIFEST '" +
            final_name + "'");
    }

    std::unique_ptr<SequentialFileWriter> writer;
    status = SequentialFileWriter::CreateNew(
        directory / temporary_name, &writer);
    if (!status.ok()) {
        return status;
    }
    status = writer->Append(encoded);
    if (status.ok()) {
        status = writer->Sync();
    }
    if (status.ok()) {
        status = writer->Close();
    }
    if (!status.ok()) {
        return status;
    }
    if (fail_before_rename_for_testing) {
        return Status::IOError("simulated MANIFEST rename failure");
    }
    status = AtomicRename(
        directory / temporary_name, directory / final_name);
    if (!status.ok()) {
        return status;
    }
    CrashPoint("manifest_after_rename_before_current_replacement");
    return SyncDirectory(directory);
}

Status InstallCurrentFile(
    const std::filesystem::path& directory,
    std::uint64_t manifest_file_number,
    bool* current_replaced,
    bool fail_before_rename_for_testing,
    bool fail_after_rename_for_testing) {
    if (current_replaced == nullptr) {
        return Status::InvalidArgument(
            "CURRENT installation received a null commit output");
    }
    *current_replaced = false;

    std::string encoded;
    Status status = EncodeCurrent(manifest_file_number, &encoded);
    if (!status.ok()) {
        return status;
    }

    const std::filesystem::path temporary =
        directory / CurrentFileName(true);
    const std::filesystem::path final =
        directory / CurrentFileName(false);
    std::unique_ptr<SequentialFileWriter> writer;
    status = SequentialFileWriter::CreateNew(temporary, &writer);
    if (!status.ok()) {
        return status;
    }
    status = writer->Append(encoded);
    if (status.ok()) {
        status = writer->Sync();
    }
    if (status.ok()) {
        status = writer->Close();
    }
    if (!status.ok()) {
        return status;
    }
    if (fail_before_rename_for_testing) {
        return Status::IOError("simulated CURRENT replacement failure");
    }
    status = AtomicRename(temporary, final);
    if (!status.ok()) {
        return status;
    }
    *current_replaced = true;
    if (fail_after_rename_for_testing) return Status::IOError("simulated post-CURRENT directory sync failure");
    CrashPoint("current_after_replacement_before_obsolete_file_deletion");
    return SyncDirectory(directory);
}

Status CleanupObsoleteFiles(
    const std::filesystem::path& directory,
    const Version& live_version) {
    std::set<std::uint64_t> live_sstables;
    for (const std::vector<FileMetaData>& level : live_version.levels) {
        for (const FileMetaData& file : level) {
            live_sstables.insert(file.file_number);
        }
    }

    std::vector<std::string> names;
    Status status = ListDirectory(directory, &names);
    if (!status.ok()) {
        return status;
    }

    bool deleted_any = false;
    for (const std::string& name : names) {
        ParsedFileName parsed;
        const Status parse_status = ParseFileName(name, &parsed);
        if (!parse_status.ok()) {
            continue;
        }

        bool remove = parsed.temporary;
        if (!parsed.temporary && parsed.kind == FileKind::kWal) {
            remove =
                parsed.file_number != live_version.active_wal_file_number &&
                parsed.file_number != live_version.immutable_wal_file_number;
        } else if (!parsed.temporary &&
                   parsed.kind == FileKind::kSstable) {
            remove = !live_sstables.contains(parsed.file_number);
        } else if (!parsed.temporary &&
                   parsed.kind == FileKind::kManifest) {
            remove =
                parsed.file_number != live_version.manifest_file_number;
        } else if (parsed.kind == FileKind::kCurrent &&
                   !parsed.temporary) {
            remove = false;
        }

        if (remove) {
            status = DeleteFileIfExists(directory / name);
            if (!status.ok()) {
                return status;
            }
            deleted_any = true;
        }
    }
    if (deleted_any) {
        return SyncDirectory(directory);
    }
    return Status::Ok();
}

}  // namespace minilsm
