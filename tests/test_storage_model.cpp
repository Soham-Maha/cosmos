// StorageModel white-box tests: the durability model only remembers what to undo, so every case
// writes through the real host file and checks the bytes after a reboot.

#include "cosmos/storage.hpp"

#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

using cosmos::kTornSectorBytes;
using cosmos::StorageModel;

namespace {

std::string make_temp_dir() {
    char pattern[] = "/tmp/cosmos-storage-model-XXXXXX";
    char* made = mkdtemp(pattern);
    assert(made != nullptr);
    return made;
}

struct TempDir {
    std::string path = make_temp_dir();
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

int open_path(const std::string& path, bool truncate = false) {
    const int flags = O_RDWR | O_CREAT | (truncate ? O_TRUNC : 0);
    const int fd = ::open(path.c_str(), flags, 0644);
    assert(fd >= 0);
    return fd;
}

void put(int fd, off_t offset, std::string_view bytes) {
    const ssize_t written = ::pwrite(fd, bytes.data(), bytes.size(), offset);
    assert(written == static_cast<ssize_t>(bytes.size()));
}

std::string get(int fd, off_t offset, size_t length) {
    std::string buffer(length, '\0');
    const ssize_t got = ::pread(fd, buffer.data(), length, offset);
    assert(got == static_cast<ssize_t>(length));
    return buffer;
}

off_t size_of(int fd) {
    struct stat info{};
    assert(::fstat(fd, &info) == 0);
    return info.st_size;
}

// 1. A write that never reached fsync is discarded, and the file shrinks back to durable size.
void discards_unsynced_write() {
    TempDir dir;
    const std::string path = dir.path + "/log";
    const int fd = open_path(path);

    StorageModel model;
    model.track_open(fd, path, false);
    assert(model.is_tracked(fd));
    assert(model.file_count() == 1);

    model.record_write(fd, 0, "", 11, 0);
    put(fd, 0, "hello world");
    model.reboot(0, false);

    assert(size_of(fd) == 0);
    assert(model.tracked_bytes() == 0);
    ::close(fd);
}

// 2. fsync is the durability boundary: the same write survives the reboot.
void keeps_fsynced_write() {
    TempDir dir;
    const std::string path = dir.path + "/log";
    const int fd = open_path(path);

    StorageModel model;
    model.track_open(fd, path, false);
    model.record_write(fd, 0, "", 5, 0);
    put(fd, 0, "abcde");
    model.commit_fsync(fd);

    model.reboot(0, false);
    assert(size_of(fd) == 5);
    assert(get(fd, 0, 5) == "abcde");
    ::close(fd);
}

// 3. A post-fsync overwrite is undone with the pre-image bytes, not just the file length.
void restores_overwritten_bytes() {
    TempDir dir;
    const std::string path = dir.path + "/state";
    const int fd = open_path(path);

    StorageModel model;
    model.track_open(fd, path, false);
    model.record_write(fd, 0, "", 5, 0);
    put(fd, 0, "AAAAA");
    model.commit_fsync(fd);

    model.record_write(fd, 1, get(fd, 1, 3), 3, 0); // pre-image "AAA"
    put(fd, 1, "BBB");
    assert(get(fd, 0, 5) == "ABBBA");

    model.reboot(0, false);
    assert(size_of(fd) == 5);
    assert(get(fd, 0, 5) == "AAAAA");
    ::close(fd);
}

// 4. A torn reboot zeroes exactly the last sector of the last synced range.
void torn_reboot_zeroes_tail_sector() {
    TempDir dir;
    const std::string path = dir.path + "/block";
    const int fd = open_path(path);
    const std::string block(1024, 'A');

    StorageModel model;
    model.track_open(fd, path, false);
    model.record_write(fd, 0, "", block.size(), 0);
    put(fd, 0, block);
    model.commit_fsync(fd);

    model.reboot(0, true);

    assert(size_of(fd) == 1024);
    assert(get(fd, 0, 1024 - kTornSectorBytes) == std::string(1024 - kTornSectorBytes, 'A'));
    assert(get(fd, 1024 - kTornSectorBytes, kTornSectorBytes) ==
           std::string(kTornSectorBytes, '\0'));
    ::close(fd);
}

// 5. O_TRUNC restarts durability at zero, so the next reboot empties the file instead of
//    restoring the length the previous run had synced.
void truncating_open_restarts_durability() {
    TempDir dir;
    const std::string path = dir.path + "/data";

    StorageModel model;
    int fd = open_path(path);
    model.track_open(fd, path, false);
    model.record_write(fd, 0, "", 5, 0);
    put(fd, 0, "hello");
    model.commit_fsync(fd);
    ::close(fd);
    model.forget_fd(fd);
    assert(!model.is_tracked(fd));

    fd = open_path(path, true);
    model.track_open(fd, path, true);
    model.record_write(fd, 0, "", 2, 0);
    put(fd, 0, "XY");
    model.reboot(0, false);

    assert(size_of(fd) == 0);
    ::close(fd);
}

// 6. set_active_node scopes each open, so rebooting one node leaves the other node's files alone.
void reboot_only_touches_its_node() {
    TempDir dir;
    const std::string first = dir.path + "/node0";
    const std::string second = dir.path + "/node1";

    StorageModel model;
    model.set_active_node(0);
    const int fd0 = open_path(first);
    model.track_open(fd0, first, false);
    model.record_write(fd0, 0, "", 4, 0);
    put(fd0, 0, "keep");

    model.set_active_node(1);
    const int fd1 = open_path(second);
    model.track_open(fd1, second, false);
    model.record_write(fd1, 0, "", 5, 1);
    put(fd1, 0, "node1");

    assert(model.active_node() == 1);
    assert(model.file_count() == 2);

    model.reboot(1, false);
    assert(get(fd0, 0, 4) == "keep");
    assert(size_of(fd1) == 0);

    model.reboot(0, false);
    assert(size_of(fd0) == 0);
    ::close(fd0);
    ::close(fd1);
}

// 7. An fd the harness never registered is invisible: no record, no durable state, no discard.
void untracked_fd_is_ignored() {
    TempDir dir;
    const std::string path = dir.path + "/plain";
    const int fd = open_path(path);

    StorageModel model;
    model.record_write(fd, 0, "old", 3, 0);
    put(fd, 0, "new");
    model.commit_fsync(fd);
    model.forget_fd(fd);

    assert(!model.is_tracked(fd));
    assert(model.file_count() == 0);
    assert(model.tracked_bytes() == 0);

    model.reboot(0, false);
    assert(get(fd, 0, 3) == "new");
    ::close(fd);
}

// 8. The pre-image budget is 32 MiB; crossing it degrades the file, and discard stops applying.
void degrades_past_preimage_budget() {
    constexpr size_t kChunk = size_t{1} << 20;
    constexpr size_t kFileChunks = 40;

    TempDir dir;
    const std::string path = dir.path + "/big";
    const int fd = open_path(path);
    assert(::ftruncate(fd, static_cast<off_t>(kChunk * kFileChunks)) == 0);

    StorageModel model;
    model.track_open(fd, path, false);

    const std::string chunk(kChunk, 'Z');
    std::string previous(kChunk, '\0');
    off_t offset = 0;
    size_t writes = 0;
    while (!model.degraded() && writes < kFileChunks) {
        assert(::pread(fd, previous.data(), previous.size(), offset) ==
               static_cast<ssize_t>(kChunk));
        model.record_write(fd, offset, previous, kChunk, 0);
        put(fd, offset, chunk);
        offset += static_cast<off_t>(kChunk);
        ++writes;
    }

    // The 33rd-megabyte pre-image crosses the budget, and the file's records are dropped.
    assert(model.degraded());
    assert(writes == 33);
    assert(model.tracked_bytes() == 0);

    // Recording on a degraded file is a no-op, and a reboot leaves the host bytes untouched.
    model.record_write(fd, 0, "abc", 3, 0);
    assert(model.tracked_bytes() == 0);
    model.reboot(0, false);

    assert(size_of(fd) == static_cast<off_t>(kChunk * kFileChunks));
    assert(get(fd, 0, 16) == std::string(16, 'Z'));
    ::close(fd);
}

// 9. A reboot must not borrow a read-only fd: restoring through it would fail and keep the
//    un-synced overwrite. The writer is the higher-numbered descriptor here.
void reboot_skips_read_only_fd() {
    TempDir dir;
    const std::string path = dir.path + "/state";
    const int reader = ::open(path.c_str(), O_RDONLY | O_CREAT, 0644);
    assert(reader >= 0);
    const int writer = open_path(path);
    assert(writer > reader);

    StorageModel model;
    model.track_open(reader, path, false);
    model.track_open(writer, path, false);
    model.record_write(writer, 0, "", 4, 0);
    put(writer, 0, "AAAA");
    model.commit_fsync(writer);

    model.record_write(writer, 0, get(writer, 0, 2), 2, 0); // pre-image "AA"
    put(writer, 0, "BB");
    assert(get(reader, 0, 4) == "BBAA");

    model.reboot(0, false);
    assert(!model.degraded());
    assert(size_of(reader) == 4);
    assert(get(reader, 0, 4) == "AAAA");
    ::close(writer);
    ::close(reader);
}

// 10. An application truncate cuts durability: a reboot keeps the new size and never resurrects
//     the bytes the cut removed.
void truncate_cuts_durability() {
    TempDir dir;
    const std::string path = dir.path + "/state";
    const int fd = open_path(path);

    StorageModel model;
    model.track_open(fd, path, false);
    model.record_write(fd, 0, "", 8, 0);
    put(fd, 0, "ABCDEFGH");
    model.commit_fsync(fd);

    model.record_truncate(fd, 4);
    assert(::ftruncate(fd, 4) == 0);
    assert(size_of(fd) == 4);

    model.reboot(0, false);
    assert(size_of(fd) == 4);
    assert(get(fd, 0, 4) == "ABCD");
    ::close(fd);
}

// 11. A growing truncate adds un-synced zeros; it must not promote the un-synced tail written
//     before the grow.
void growing_truncate_stays_undurable() {
    TempDir dir;
    const std::string path = dir.path + "/state";
    const int fd = open_path(path);

    StorageModel model;
    model.track_open(fd, path, false);
    model.record_write(fd, 0, "", 4, 0);
    put(fd, 0, "AAAA");
    model.commit_fsync(fd);

    model.record_write(fd, 4, "", 4, 0);
    put(fd, 4, "BBBB");
    assert(::ftruncate(fd, 8) == 0); // already 8 bytes: growth is a no-op
    model.record_truncate(fd, 8);

    model.reboot(0, false);
    assert(size_of(fd) == 4);
    assert(get(fd, 0, 4) == "AAAA");
    ::close(fd);
}

// 12. unlink+recreate behind the model's back shrinks durable size on the next open; the stale
//     length the old file synced is not restored.
void recreating_shrinks_durable_size() {
    TempDir dir;
    const std::string path = dir.path + "/state";
    StorageModel model;

    int fd = open_path(path);
    model.track_open(fd, path, false);
    model.record_write(fd, 0, "", 8, 0);
    put(fd, 0, "ABCDEFGH");
    model.commit_fsync(fd);
    ::close(fd);
    model.forget_fd(fd);

    assert(::unlink(path.c_str()) == 0);
    fd = open_path(path); // fresh file, size 0
    model.track_open(fd, path, false);

    model.reboot(0, false);
    assert(model.file_count() == 1);
    assert(size_of(fd) == 0);
    ::close(fd);
}

// I6: a path reopened under a different active node belongs to the node that opened it last.
void reopening_reattributes_the_node() {
    TempDir dir;
    const std::string path = dir.path + "/shared";

    StorageModel model;
    model.set_active_node(0);
    int fd = open_path(path, true);
    model.track_open(fd, path, true);
    model.record_write(fd, 0, "", 4, 0);
    put(fd, 0, "AAAA");
    model.commit_fsync(fd);
    ::close(fd);
    model.forget_fd(fd);

    model.set_active_node(1);
    fd = open_path(path);
    model.track_open(fd, path, false);
    model.record_write(fd, 0, "AA", 2, 1);
    put(fd, 0, "BB");

    // Node 0 no longer owns the path, so its reboot must leave the un-synced bytes alone.
    model.reboot(0, false);
    assert(get(fd, 0, 4) == "BBAA");

    model.reboot(1, false);
    assert(get(fd, 0, 4) == "AAAA");
    ::close(fd);
}

// A read-only open must not re-home a file to whatever node happened to be active.
void read_open_does_not_rehome_a_file() {
    TempDir dir;
    const std::string path = dir.path + "/reader";

    StorageModel model;
    model.set_active_node(2);
    const int writer = open_path(path, true);
    model.track_open(writer, path, true);
    model.record_write(writer, 0, "", 4, 2);
    put(writer, 0, "AAAA");
    model.commit_fsync(writer);
    model.record_write(writer, 0, "AA", 2, 2);
    put(writer, 0, "BB");

    model.set_active_node(0); // stale for the reader below
    const int reader = ::open(path.c_str(), O_RDONLY);
    assert(reader >= 0);
    model.track_open(reader, path, false);

    model.reboot(0, false); // not the owner: node 2's un-synced bytes survive
    assert(get(reader, 0, 4) == "BBAA");

    model.reboot(2, false); // the writer's reboot discards them
    assert(get(reader, 0, 4) == "AAAA");
    ::close(reader);
}

// B4: the last writer owns a shared fd; rebooting the opener must not discard the writer's bytes.
void later_writer_owns_the_file() {
    TempDir dir;
    const std::string path = dir.path + "/shared-fd";

    StorageModel model;
    model.set_active_node(0);
    const int fd = open_path(path, true);
    model.track_open(fd, path, true);
    model.record_write(fd, 0, "", 4, 0);
    put(fd, 0, "AAAA");
    model.commit_fsync(fd);

    model.record_write(fd, 0, "AA", 2, 1); // node 1 writes through node 0's fd
    put(fd, 0, "BB");

    model.reboot(0, false); // no longer the owner
    assert(get(fd, 0, 4) == "BBAA");

    model.reboot(1, false); // the writer's reboot discards its own unsynced bytes
    assert(get(fd, 0, 4) == "AAAA");
    ::close(fd);
}

// I7: a failing fstat is not reproducible without a mock, so the untracked-on-failure path has no
// runtime test here.

} // namespace

int main() {
    discards_unsynced_write();
    keeps_fsynced_write();
    restores_overwritten_bytes();
    torn_reboot_zeroes_tail_sector();
    truncating_open_restarts_durability();
    reboot_only_touches_its_node();
    untracked_fd_is_ignored();
    degrades_past_preimage_budget();
    reboot_skips_read_only_fd();
    truncate_cuts_durability();
    growing_truncate_stays_undurable();
    recreating_shrinks_durable_size();
    reopening_reattributes_the_node();
    later_writer_owns_the_file();
    read_open_does_not_rehome_a_file();
    return 0;
}
