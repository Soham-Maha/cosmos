/*
 * A deliberately broken example application, used by tests/test_broken_app.cpp.
 *
 * Plain C with no knowledge of libcosmos: the simulator reaches it only through -Wl,--wrap on the
 * allocator, exactly as it would reach any application whose source nobody edited.
 *
 * The defect lives in cache_put and is described there. Compile with
 * -DBROKEN_CACHE_CHECKS_MALLOC to build the fixed version.
 */

#ifndef COSMOS_EXAMPLE_BROKEN_CACHE_H
#define COSMOS_EXAMPLE_BROKEN_CACHE_H

#include <stddef.h>

#define CACHE_CAPACITY 64
#define CACHE_KEY_MAX 24
#define CACHE_VALUE_MAX 32

#define CACHE_OK 0
#define CACHE_FULL (-1)
#define CACHE_NO_MEMORY (-2)

typedef struct Cache Cache;

#ifdef __cplusplus
extern "C" {
#endif

/* NULL if the cache itself could not be allocated. */
Cache* cache_create(void);
void cache_destroy(Cache* cache);

/* CACHE_OK means the caller may rely on cache_get returning this value afterwards. */
int cache_put(Cache* cache, const char* key, const char* value);

/* NULL when the key is absent. */
const char* cache_get(const Cache* cache, const char* key);

size_t cache_size(const Cache* cache);

#ifdef __cplusplus
}
#endif

#endif
