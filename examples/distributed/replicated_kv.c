/*
 * Primary-backup replicated KV store (docs/fault-injection.md §17).
 *
 * Node 0 accepts client writes and forwards each to the followers. The buggy build acks the
 * client after *sending* to the followers, without waiting for an acknowledgement: a partition or
 * a crash can therefore leave an acked write on no backup at all. Build with
 * -DRKV_REQUIRE_BACKUP_ACK for the fixed variant, which acks only after one backup has applied it.
 *
 * Standard POSIX C: sockets, pthreads, virtual time. Under libcosmos a whole cluster runs as
 * fibers in one universe, one fiber per node.
 */
#define _POSIX_C_SOURCE 200809L

#include "replicated_kv.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RKV_MAX_ENTRIES 32

struct rkv_frame {
    char cmd; /* 'P' put, 'S' stop, 'A' ack, 'N' nak */
    char key[RKV_KEY_MAX];
    char value[RKV_VALUE_MAX];
};

static char g_key[RKV_MAX_NODES][RKV_MAX_ENTRIES][RKV_KEY_MAX];
static char g_value[RKV_MAX_NODES][RKV_MAX_ENTRIES][RKV_VALUE_MAX];
static unsigned char g_used[RKV_MAX_NODES][RKV_MAX_ENTRIES];
static int g_listen_fd[RKV_MAX_NODES];
static int g_peer_fd[RKV_MAX_NODES];
static int g_shutdown;

void rkv_shutdown(void) {
    g_shutdown = 1;
    for (int n = 0; n < RKV_MAX_NODES; ++n) {
        if (g_peer_fd[n] >= 0) {
            close(g_peer_fd[n]);
            g_peer_fd[n] = -1;
        }
        if (g_listen_fd[n] >= 0) {
            close(g_listen_fd[n]);
            g_listen_fd[n] = -1;
        }
    }
}

static void store(int node, const char* key, const char* value) {
    for (int i = 0; i < RKV_MAX_ENTRIES; ++i) {
        if (g_used[node][i] && strcmp(g_key[node][i], key) == 0) {
            snprintf(g_value[node][i], RKV_VALUE_MAX, "%s", value);
            return;
        }
    }
    for (int i = 0; i < RKV_MAX_ENTRIES; ++i) {
        if (!g_used[node][i]) {
            snprintf(g_key[node][i], RKV_KEY_MAX, "%s", key);
            snprintf(g_value[node][i], RKV_VALUE_MAX, "%s", value);
            g_used[node][i] = 1;
            return;
        }
    }
}

int rkv_node_has(int node, const char* key, const char* value) {
    if (node < 0 || node >= RKV_MAX_NODES) return 0;
    for (int i = 0; i < RKV_MAX_ENTRIES; ++i) {
        if (!g_used[node][i] || strcmp(g_key[node][i], key) != 0) continue;
        return value == NULL || strcmp(g_value[node][i], value) == 0;
    }
    return 0;
}

void rkv_reset(void) {
    memset(g_used, 0, sizeof(g_used));
    memset(g_key, 0, sizeof(g_key));
    memset(g_value, 0, sizeof(g_value));
    for (int n = 0; n < RKV_MAX_NODES; ++n) {
        g_listen_fd[n] = -1;
        g_peer_fd[n] = -1;
    }
    g_shutdown = 0;
}

static int send_all(int fd, const void* data, size_t len) {
    const char* bytes = (const char*)(data);
    while (len > 0) {
        const ssize_t sent = send(fd, bytes, len, 0);
        if (sent <= 0) return -1;
        bytes += sent;
        len -= (size_t)(sent);
    }
    return 0;
}

static int recv_all(int fd, void* data, size_t len) {
    char* bytes = (char*)(data);
    while (len > 0) {
        const ssize_t got = recv(fd, bytes, len, 0);
        if (got <= 0) return -1;
        bytes += got;
        len -= (size_t)(got);
    }
    return 0;
}

static int listen_on(int port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (const struct sockaddr*)(&addr), sizeof(addr)) != 0 || listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int connect_retry(int port) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons((unsigned short)(port));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, (const struct sockaddr*)(&addr), sizeof(addr)) == 0) {
            return fd;
        }
        close(fd);
        const struct timespec pause = {0, 200000};
        nanosleep(&pause, NULL);
    }
    return -1;
}

#ifdef RKV_REQUIRE_BACKUP_ACK
/* Waits briefly for one backup ack; a dropped packet must not hang the primary. */
static int await_ack(int fd) {
    for (int spin = 0; spin < 200; ++spin) {
        char answer = 0;
        const ssize_t got = recv(fd, &answer, 1, 0);
        if (got == 1) return answer == 'A';
        if (got == 0) return 0;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return 0;
        sched_yield();
    }
    return 0;
}
#endif

static int primary_main(int nodes, int base_port) {
    if (g_shutdown) return 0;
    int followers[RKV_MAX_NODES];
    for (int n = 0; n < RKV_MAX_NODES; ++n)
        followers[n] = -1;
    for (int n = 1; n < nodes; ++n) {
        followers[n] = connect_retry(base_port + n);
        if (followers[n] >= 0) {
            const int flags = fcntl(followers[n], F_GETFL, 0);
            (void)fcntl(followers[n], F_SETFL, flags | O_NONBLOCK);
        }
    }
    if (g_shutdown) return 0;
    const int listener = listen_on(base_port);
    if (listener < 0) return -1;
    g_listen_fd[0] = listener;
    if (g_shutdown) {
        close(listener);
        g_listen_fd[0] = -1;
        return 0;
    }

    for (;;) {
        const int client = accept(listener, NULL, NULL);
        if (client < 0) return 0;
        g_peer_fd[0] = client;
        for (;;) {
            struct rkv_frame frame;
            if (recv_all(client, &frame, sizeof(frame)) != 0) break;
            if (frame.cmd != 'P') continue;

            store(0, frame.key, frame.value);
#ifdef RKV_REQUIRE_BACKUP_ACK
            int sent[RKV_MAX_NODES] = {0};
#endif
            for (int n = 1; n < nodes; ++n) {
                if (followers[n] < 0) continue;
                const int sent_ok = send_all(followers[n], &frame, sizeof(frame)) == 0;
#ifdef RKV_REQUIRE_BACKUP_ACK
                sent[n] = sent_ok;
#endif
                if (!sent_ok) {
                    close(followers[n]);
                    followers[n] = -1;
                }
            }
            char answer = 'A';
#ifdef RKV_REQUIRE_BACKUP_ACK
            int acked = 0;
            for (int n = 1; n < nodes; ++n) {
                if (sent[n] && await_ack(followers[n])) ++acked;
            }
            if (acked == 0) answer = 'N';
#endif
            if (send_all(client, &answer, 1) != 0) break;
        }
        g_peer_fd[0] = -1;
        close(client);
    }
}

static int follower_main(int node, int base_port) {
    if (g_shutdown) return 0;
    if (g_shutdown) return 0;
    const int listener = listen_on(base_port + node);
    if (listener < 0) return -1;
    g_listen_fd[node] = listener;
    if (g_shutdown) {
        close(listener);
        g_listen_fd[node] = -1;
        return 0;
    }
    const int primary = accept(listener, NULL, NULL);
    if (primary < 0) return 0;
    g_peer_fd[node] = primary;
    for (;;) {
        struct rkv_frame frame;
        if (recv_all(primary, &frame, sizeof(frame)) != 0) break;
        if (frame.cmd != 'P') continue;
        store(node, frame.key, frame.value);
        const char answer = 'A';
        if (send_all(primary, &answer, 1) != 0) break;
    }
    g_peer_fd[node] = -1;
    close(primary);
    close(listener);
    return 0;
}

int rkv_node_main(int node, int nodes, int base_port) {
    if (node == 0) return primary_main(nodes, base_port);
    return follower_main(node, base_port);
}

int rkv_client_put(int base_port, const char* key, const char* value) {
    const int fd = connect_retry(base_port);
    if (fd < 0) return -1;
    struct rkv_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.cmd = 'P';
    snprintf(frame.key, RKV_KEY_MAX, "%s", key);
    snprintf(frame.value, RKV_VALUE_MAX, "%s", value);
    int rc = -1;
    if (send_all(fd, &frame, sizeof(frame)) == 0) {
        char answer = 0;
        if (recv_all(fd, &answer, 1) == 0 && answer == 'A') rc = 0;
    }
    close(fd);
    return rc;
}

#ifndef COSMOS_RKV_NO_MAIN
struct rkv_thread_arg {
    int node;
    int nodes;
    int base_port;
};

static void* rkv_thread(void* raw) {
    struct rkv_thread_arg* arg = (struct rkv_thread_arg*)(raw);
    (void)rkv_node_main(arg->node, arg->nodes, arg->base_port);
    return NULL;
}

int main(void) {
    rkv_reset();
    pthread_t threads[RKV_MAX_NODES];
    struct rkv_thread_arg args[RKV_MAX_NODES];
    for (int n = 0; n < RKV_MAX_NODES; ++n) {
        args[n] = (struct rkv_thread_arg){n, RKV_MAX_NODES, RKV_BASE_PORT};
        if (pthread_create(&threads[n], NULL, rkv_thread, &args[n]) != 0) return 1;
    }
    const struct timespec settle = {0, 200000000};
    nanosleep(&settle, NULL);
    const int rc = rkv_client_put(RKV_BASE_PORT, "hello", "world");
    printf("replicated_kv: put %s (rc=%d), backups: n1=%d n2=%d\n", rc == 0 ? "acked" : "refused",
           rc, rkv_node_has(1, "hello", "world"), rkv_node_has(2, "hello", "world"));
    rkv_shutdown();
    for (int n = 0; n < RKV_MAX_NODES; ++n)
        (void)pthread_join(threads[n], NULL);
    return 0;
}
#endif
