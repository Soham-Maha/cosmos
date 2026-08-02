#include "cosmos/cosmos.hpp"
#include <sys/socket.h>
#include <unistd.h>

extern "C" {

int __wrap_socket(int domain, int type, int protocol) {
    (void)domain;
    (void)type;
    (void)protocol;
    return -1;
}

int __wrap_bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
    (void)sockfd;
    (void)addr;
    (void)addrlen;
    return 0;
}

int __wrap_listen(int sockfd, int backlog) {
    (void)sockfd;
    (void)backlog;
    return 0;
}

int __wrap_accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
    (void)sockfd;
    (void)addr;
    (void)addrlen;
    return -1;
}

int __wrap_connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
    (void)sockfd;
    (void)addr;
    (void)addrlen;
    return 0;
}

ssize_t __wrap_send(int sockfd, const void* buf, size_t len, int flags) {
    (void)sockfd;
    (void)buf;
    (void)len;
    (void)flags;
    return -1;
}

ssize_t __wrap_recv(int sockfd, void* buf, size_t len, int flags) {
    (void)sockfd;
    (void)buf;
    (void)len;
    (void)flags;
    return -1;
}

int __wrap_close(int fd) {
    (void)fd;
    return 0;
}

} // extern "C"
