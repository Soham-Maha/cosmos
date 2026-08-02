#include "cosmos/cosmos.hpp"
#include <sys/socket.h>
#include <unistd.h>

// Linker-wrapping passthrough stubs for the POSIX network socket surface
// (see docs/design.md §3 "Network"). The in-process simulated topology /
// latency-loss-reorder-partition faults are layered on later; for now they
// pass through to the real kernel sockets via __real_*.

extern "C" {

int     __real_socket(int domain, int type, int protocol);
int     __real_bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen);
int     __real_listen(int sockfd, int backlog);
int     __real_accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen);
int     __real_connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen);
ssize_t __real_send(int sockfd, const void* buf, size_t len, int flags);
ssize_t __real_recv(int sockfd, void* buf, size_t len, int flags);
int     __real_close(int fd);

int __wrap_socket(int domain, int type, int protocol) {
    return __real_socket(domain, type, protocol);
}

int __wrap_bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
    return __real_bind(sockfd, addr, addrlen);
}

int __wrap_listen(int sockfd, int backlog) {
    return __real_listen(sockfd, backlog);
}

int __wrap_accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
    return __real_accept(sockfd, addr, addrlen);
}

int __wrap_connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
    return __real_connect(sockfd, addr, addrlen);
}

ssize_t __wrap_send(int sockfd, const void* buf, size_t len, int flags) {
    return __real_send(sockfd, buf, len, flags);
}

ssize_t __wrap_recv(int sockfd, void* buf, size_t len, int flags) {
    return __real_recv(sockfd, buf, len, flags);
}

int __wrap_close(int fd) {
    return __real_close(fd);
}

} // extern "C"
