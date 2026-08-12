#include "cosmos/cosmos.hpp"
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

// Linker-wrapping passthrough stubs for the POSIX storage surface
// (see docs/design.md §3 "Storage"). The simulated page cache / torn-write /
// fsync durability model is layered on later; for now they pass through to the
// real host filesystem via __real_*.

extern "C" {

int __real_open(const char* pathname, int flags, ...);
ssize_t __real_read(int fd, void* buf, size_t count);
ssize_t __real_write(int fd, const void* buf, size_t count);
int __real_fsync(int fd);

// open(2) is variadic: the optional mode argument is only meaningful when
// O_CREAT (or O_TMPFILE) is set. Forward it correctly to the real impl.
int __wrap_open(const char* pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }
    return __real_open(pathname, flags, mode);
}

ssize_t __wrap_read(int fd, void* buf, size_t count) { return __real_read(fd, buf, count); }

ssize_t __wrap_write(int fd, const void* buf, size_t count) { return __real_write(fd, buf, count); }

int __wrap_fsync(int fd) { return __real_fsync(fd); }

} // extern "C"
