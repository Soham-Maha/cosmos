#include "cosmos/storage.hpp"

#include <algorithm>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

// The bytes stay in the host file; this model only remembers how to put the old ones back after
// a reboot. Its syscalls are openat, pwrite, fstat, ftruncate, close and fcntl. close and fcntl
// are link-wrapped, but neither re-enters the injector: __wrap_close only forgets the fd (it
// draws no fault, so a model close is benign) and __wrap_fcntl passes a host fd straight to the
// real call. Degradation stays sticky: once a file or the universe is degraded, discard for it is
// a no-op for the rest of the run.

namespace cosmos {

namespace {

// Reports the kernel size; false when fstat fails, because "unknown" must not read as "empty".
bool size_of(int fd, off_t& size) {
    struct stat info{};
    if (fstat(fd, &info) != 0) return false;
    size = info.st_size;
    return true;
}

// Only a descriptor that may write can put the old bytes back.
bool writable(int fd) {
    const int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return false;
    const int mode = flags & O_ACCMODE;
    return mode == O_WRONLY || mode == O_RDWR;
}

// Raw syscalls: a test binary may link --wrap=pwrite/ftruncate, which also rewrites libcosmos's
// own references. A rollback that re-entered the injector would draw a fault and record writes
// into the very vector it is draining.
ssize_t host_pwrite(int fd, const void* buffer, size_t size, off_t offset) {
    return static_cast<ssize_t>(::syscall(SYS_pwrite64, fd, buffer, size, offset));
}

int host_ftruncate(int fd, off_t size) {
    return static_cast<int>(::syscall(SYS_ftruncate, fd, size));
}

} // namespace

void StorageModel::track_open(int fd, std::string path, bool truncate) {
    off_t actual = 0;
    if (!size_of(fd, actual)) {
        // Treating a failed fstat as zero would let a reboot empty a non-empty file.
        fd_paths_.erase(fd);
        auto known = files_.find(path);
        if (known != files_.end()) known->second.degraded = true;
        degraded_ = true;
        return;
    }

    // A reused fd stops pointing at whatever path it held before.
    fd_paths_[fd] = path;

    auto [entry, inserted] = files_.try_emplace(std::move(path));
    FileState& file = entry->second;
    if (inserted) {
        file.durable_size = actual;
    } else if (actual < file.durable_size) {
        // The file shrank behind the model's back (unlink+recreate, external truncate).
        drop_unsynced(file);
        file.last_sync.reset();
        file.durable_size = actual;
    }
    // Never raise durable_size here: another fd's un-synced append is not durable.
    file.node = active_node_;

    if (truncate) {
        // O_TRUNC threw the previous contents away, so the durable state is empty too.
        drop_unsynced(file);
        file.durable_size = 0;
        file.last_sync.reset();
    }
}

void StorageModel::forget_fd(int fd) { fd_paths_.erase(fd); }

void StorageModel::record_write(int fd, off_t offset, std::string_view previous, size_t length) {
    auto fd_entry = fd_paths_.find(fd);
    if (fd_entry == fd_paths_.end()) return;
    auto file_entry = files_.find(fd_entry->second);
    if (file_entry == files_.end()) return;

    FileState& file = file_entry->second;
    if (file.degraded) return; // discard is already a no-op for this file

    file.unsynced.push_back(WriteRecord{offset, length, std::string(previous)});
    tracked_bytes_ += previous.size();

    if (tracked_bytes_ > kMaxTrackedBytes) {
        // Past the budget the file stops being discard-tracked: holding half a history would
        // restore a state the crash never produced.
        degraded_ = true;
        file.degraded = true;
        drop_unsynced(file);
    }
}

void StorageModel::commit_fsync(int fd) {
    auto fd_entry = fd_paths_.find(fd);
    if (fd_entry == fd_paths_.end()) return;
    auto file_entry = files_.find(fd_entry->second);
    if (file_entry == files_.end()) return;

    FileState& file = file_entry->second;
    off_t actual = 0;
    if (!size_of(fd, actual)) {
        // Without the real size the fsync boundary is unknown; stop simulating discard.
        degraded_ = true;
        file.degraded = true;
        drop_unsynced(file);
        file.last_sync.reset();
        return;
    }
    // fsync's boundary is the file's size as the kernel reports it.
    file.durable_size = actual;
    if (!file.unsynced.empty()) file.last_sync = file.unsynced.back();
    drop_unsynced(file);
}

void StorageModel::record_truncate(int fd, off_t new_size) {
    auto fd_entry = fd_paths_.find(fd);
    if (fd_entry == fd_paths_.end()) return;
    auto file_entry = files_.find(fd_entry->second);
    if (file_entry == files_.end()) return;

    FileState& file = file_entry->second;
    // A record starting at or past the cut can never be rolled back into existence.
    std::erase_if(file.unsynced, [this, new_size](const WriteRecord& record) {
        if (record.offset < new_size) return false;
        tracked_bytes_ -= record.previous.size();
        return true;
    });

    // Growth only adds un-synced zeros; it never makes an earlier write durable.
    if (new_size >= file.durable_size) return;

    if (file.last_sync.has_value()) {
        WriteRecord& last = *file.last_sync;
        if (last.offset >= new_size) {
            file.last_sync.reset();
        } else if (last.offset + static_cast<off_t>(last.length) > new_size) {
            // A torn reboot may only zero what is still inside the file.
            last.length = static_cast<size_t>(new_size - last.offset);
        }
    }
    file.durable_size = new_size;
}

void StorageModel::reboot(NodeId node, bool torn) {
    for (auto& [path, file] : files_) {
        if (file.node != node) continue;
        if (file.degraded) continue; // nothing was tracked, so nothing can be discarded

        // An fd the application already holds is reused only if it may write; a read-only one
        // would fail every pwrite and silently keep the un-synced bytes.
        int fd = -1;
        for (const auto& [open_fd, open_path] : fd_paths_) {
            if (open_path != path || !writable(open_fd)) continue;
            fd = open_fd;
            break;
        }
        const bool borrowed = fd >= 0;
        rollback(file, fd, path);
        if (fd < 0) {
            // The file is gone or unopenable, so its bytes cannot be un-written.
            drop_unsynced(file);
            file.last_sync.reset();
            continue;
        }

        if (!file.degraded && torn && file.last_sync.has_value() && file.last_sync->length > 0) {
            // An appended sector has no older bytes to fall back to, so tear its tail with zeros.
            const size_t tear = std::min(kTornSectorBytes, file.last_sync->length);
            const std::vector<char> zeros(tear, '\0');
            const off_t at =
                file.last_sync->offset + static_cast<off_t>(file.last_sync->length - tear);
            if (host_pwrite(fd, zeros.data(), zeros.size(), at) !=
                static_cast<ssize_t>(zeros.size())) {
                degraded_ = true;
                file.degraded = true;
            }
        }

        if (!borrowed) (void)close(fd);
        drop_unsynced(file);
        file.last_sync.reset();
    }
}

void StorageModel::rollback(FileState& file, int& fd, const std::string& path) {
    if (fd < 0) fd = openat(AT_FDCWD, path.c_str(), O_RDWR);
    if (fd < 0) {
        // The model cannot restore this file; stop pretending it can.
        degraded_ = true;
        file.degraded = true;
        return;
    }

    // Newest first, so overlapping writes leave the oldest pre-image in place.
    for (auto record = file.unsynced.rbegin(); record != file.unsynced.rend(); ++record) {
        if (record->previous.empty()) continue;
        if (host_pwrite(fd, record->previous.data(), record->previous.size(), record->offset) !=
            static_cast<ssize_t>(record->previous.size())) {
            // A partial restore is not the state the crash produced.
            degraded_ = true;
            file.degraded = true;
            return;
        }
    }
    // Drops bytes appended past the last fsync, including zero-filled holes.
    if (host_ftruncate(fd, file.durable_size) != 0) {
        degraded_ = true;
        file.degraded = true;
    }
}

void StorageModel::drop_unsynced(FileState& file) {
    for (const WriteRecord& record : file.unsynced)
        tracked_bytes_ -= record.previous.size();
    file.unsynced.clear();
}

} // namespace cosmos
