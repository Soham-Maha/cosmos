#ifndef COSMOS_REPLICATED_KV_H
#define COSMOS_REPLICATED_KV_H

#include <stddef.h>

#define RKV_MAX_NODES 3
#define RKV_KEY_MAX 24
#define RKV_VALUE_MAX 64
#define RKV_BASE_PORT 19000

#ifdef __cplusplus
extern "C" {
#endif

/* Clears every replica table; call before a run. */
void rkv_reset(void);

/* One replica main loop: node 0 is the primary, 1..nodes-1 are followers. */
int rkv_node_main(int node, int nodes, int base_port);

/* Client write, on its own connection to the primary. */
int rkv_client_put(int base_port, const char* key, const char* value);

/* Closes every node's sockets, waking blocked fibers; call before joining them. */
void rkv_shutdown(void);

/* Oracle read of one replica's in-memory table (same process). */
int rkv_node_has(int node, const char* key, const char* value);

#ifdef __cplusplus
}
#endif

#endif
