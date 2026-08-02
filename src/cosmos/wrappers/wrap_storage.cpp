#include "cosmos/cosmos.hpp"
#include <unistd.h>
#include <fcntl.h>

extern "C" {

int __wrap_open(const char* pathname, int flags, ...) {
    (void)pathname;
    (void)flags;
    return -1;
}

ssize_t __wrap_read(int fd, void* buf, size_t count) {
    (void)fd;
    (void)buf;
    (void)count;
    return -1;
}

ssize_t __wrap_write(int fd, const void* buf, size_t count) {
    (void)fd;
    (void)buf;
    (void)count;
    return -1;
}

int __wrap_fsync(int fd) {
    (void)fd;
    return 0;
}

} // extern "C"
