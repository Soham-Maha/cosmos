#pragma once

#include "cosmos/faults.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace cosmos {

// Simulated durability (docs/design.md §8): a write lands in an un-synced page cache, fsync makes
// it durable, and a reboot discards whatever was not. Bytes stay in the host file so lseek and
// ftruncate keep native semantics; the model only remembers what to put back.
class StorageModel {
  public:
    // Pre-image bytes held per universe; past this the model stops simulating discard for a file.
    static constexpr size_t kMaxTrackedBytes = 32u << 20;

    // Node that later opens belong to. Single-node harnesses never set it; P6 uses it per node.
    void set_active_node(NodeId node) { active_node_ = node; }
    NodeId active_node() const { return active_node_; }

    // Registers an fd after a successful open. truncate means O_TRUNC: durability restarts empty.
    // Reopening a known path refreshes its node and shrinks durable size when the file shrank
    // behind the model's back (unlink+recreate, external truncate).
    void track_open(int fd, std::string path, bool truncate);
    void forget_fd(int fd);
    bool is_tracked(int fd) const { return fd_paths_.contains(fd); }

    // Copies the bytes a write is about to overwrite, so a reboot can put them back. length is
    // what the write itself transferred, which a torn sector zeroes even when it added bytes.
    void record_write(int fd, off_t offset, std::string_view previous, size_t length);
    // fsync's boundary: the file's current size becomes durable and rollback records are dropped.
    void commit_fsync(int fd);
    // An application ftruncate can only lower durability: a cut drops the records past it, and a
    // growing truncate never promotes bytes that were written without fsync.
    void record_truncate(int fd, off_t new_size);

    // Discards un-synced writes for every file owned by node. torn zeroes the last synced sector,
    // because an appended sector has no older bytes to fall back to (the pre-image is empty).
    void reboot(NodeId node, bool torn);

    // True once any file outgrew kMaxTrackedBytes; discard is then a no-op for that file. Sticky
    // for the universe's lifetime: a budget that was exceeded is a fact about the run.
    bool degraded() const { return degraded_; }
    size_t tracked_bytes() const { return tracked_bytes_; }
    size_t file_count() const { return files_.size(); }

  private:
    // One write: where it started, how many bytes it wrote, and the bytes that were there.
    struct WriteRecord {
        off_t offset = 0;
        size_t length = 0;
        std::string previous;
    };

    struct FileState {
        NodeId node = 0;
        off_t durable_size = 0;
        std::vector<WriteRecord> unsynced;
        // The last range fsync committed, kept only so a torn reboot can revert its tail.
        std::optional<WriteRecord> last_sync;
        bool degraded = false;
    };

    void rollback(FileState& file, int& fd, const std::string& path);
    void drop_unsynced(FileState& file);

    std::map<std::string, FileState> files_;
    std::map<int, std::string> fd_paths_;
    size_t tracked_bytes_{0};
    NodeId active_node_{0};
    bool degraded_{false};
};

// The bytes a torn reboot zeroes at the end of the last synced range.
inline constexpr size_t kTornSectorBytes = 512;

} // namespace cosmos
