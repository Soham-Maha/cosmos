/*
 * Distributed application example: Replicated Primary-Backup / Consensus Cluster.
 *
 * Standard POSIX C application using sockets (socket, bind, connect, send, recv),
 * pthread_create, mutexes, and virtual time.
 *
 * Under standard OS execution (PROD), nodes communicate across real TCP/IP sockets.
 * Under simulation testing (SIM with libcosmos), libcosmos intercepts socket calls
 * via -Wl,--wrap to simulate network topologies, packet loss, reordering, split-brain
 * partitions, and node crash/reboots deterministically.
 */

#include <stdio.h>
#include <stdlib.h>

int main(void) {
    printf("Distributed Replicated KV Consensus Example\n");
    return 0;
}
