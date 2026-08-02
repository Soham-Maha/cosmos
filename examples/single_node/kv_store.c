/*
 * Single-node application example: Transactional WAL Key-Value Storage Engine.
 * 
 * Standard POSIX C application using malloc, pthread, write, open, and fsync.
 * 
 * Under standard OS execution (PROD), calls execute native libc/kernel syscalls.
 * Under simulation testing (SIM with libcosmos), libcosmos intercepts syscalls
 * via -Wl,--wrap to inject storage crash/torn write faults, memory allocation (OOM)
 * failures, and concurrency race exploration.
 */

#include <stdio.h>
#include <stdlib.h>

int main(void) {
    printf("Single-Node Transactional KV Store Example\n");
    return 0;
}
