/*
 * A small write-ahead-log key-value store, used by tests/test_kv_store.cpp.
 *
 * Plain C with no knowledge of libcosmos: the simulator reaches it only through -Wl,--wrap on
 * open/read/write/fsync/close, exactly as it would reach any application whose source nobody
 * edited. The durability contract lives in kv_put: a put is acknowledged only once the record is
 * in the host file, and -- when the caller asks for it -- only once fsync has made it durable.
 *
 * The log is deliberately the whole storage engine: one append-only file, replayed from the start
 * on open, so a crash can only ever cost a prefix of what was written.
 */

#ifndef COSMOS_EXAMPLE_KV_STORE_H
#define COSMOS_EXAMPLE_KV_STORE_H

#include <stddef.h>

/* Fixed limits instead of a growable table: the example has to stay small enough to read in one
   sitting, and every record's size is then known before it is written. */
#define KV_MAX_ENTRIES 64
#define KV_KEY_MAX 32
#define KV_VALUE_MAX 128

/* Name of the log inside the directory handed to kv_open. */
#define KV_LOG_NAME "wal.log"

#define KV_OK 0
#define KV_ERR (-1)

typedef struct kv_store kv_store;

#ifdef __cplusplus
extern "C" {
#endif

/* Opens <dir>/wal.log, replaying its records in order. NULL if the directory is unusable, the log
   cannot be opened, or the store cannot be allocated. */
kv_store* kv_open(const char* dir);

/* Appends one record and then acknowledges it in the in-memory table. durable asks for an fsync
   before the ack; without it the value survives until the page cache does. Returns KV_ERR, and
   leaves the table untouched, when the record was not written in full or the fsync failed. */
int kv_put(kv_store* store, const char* key, const char* value, int durable);

/* Copies the value into out, NUL-terminated. KV_ERR when the key is absent or cap cannot hold the
   value plus its terminator. */
int kv_get(kv_store* store, const char* key, char* out, size_t cap);

void kv_close(kv_store* store);

#ifdef __cplusplus
}
#endif

#endif
