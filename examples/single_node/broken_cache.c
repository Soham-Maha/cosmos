#include "broken_cache.h"

#include <stdlib.h>
#include <string.h>

struct Entry {
    char key[CACHE_KEY_MAX];
    char value[CACHE_VALUE_MAX];
};

struct Cache {
    struct Entry* slots[CACHE_CAPACITY];
    size_t count;
};

Cache* cache_create(void) {
    Cache* cache = malloc(sizeof(*cache));
    if (cache == NULL) {
        return NULL;
    }
    memset(cache, 0, sizeof(*cache));
    return cache;
}

void cache_destroy(Cache* cache) {
    if (cache == NULL) {
        return;
    }
    for (size_t i = 0; i < cache->count; ++i) {
        free(cache->slots[i]);
    }
    free(cache);
}

/* Hand-rolled rather than snprintf so the only allocations this file makes are its own mallocs:
   the harness counts eligible calls exactly, and stdio is free to allocate internally. */
static void copy_bounded(char* dst, size_t cap, const char* src) {
    size_t i = 0;
    for (; i + 1 < cap && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static struct Entry* find(const Cache* cache, const char* key) {
    for (size_t i = 0; i < cache->count; ++i) {
        if (strcmp(cache->slots[i]->key, key) == 0) {
            return cache->slots[i];
        }
    }
    return NULL;
}

int cache_put(Cache* cache, const char* key, const char* value) {
    struct Entry* existing = find(cache, key);
    if (existing != NULL) {
        copy_bounded(existing->value, CACHE_VALUE_MAX, value);
        return CACHE_OK;
    }

    if (cache->count == CACHE_CAPACITY) {
        return CACHE_FULL;
    }

    struct Entry* entry = malloc(sizeof(*entry));
    if (entry == NULL) {
#ifdef BROKEN_CACHE_CHECKS_MALLOC
        return CACHE_NO_MEMORY;
#else
        /*
         * The defect. The allocation failure is noticed and then discarded: the caller is told the
         * value was stored, so a later cache_get misses a key the application believes is present.
         * Nothing crashes and nothing is logged, which is what makes it worth finding.
         */
        return CACHE_OK;
#endif
    }

    copy_bounded(entry->key, CACHE_KEY_MAX, key);
    copy_bounded(entry->value, CACHE_VALUE_MAX, value);
    cache->slots[cache->count] = entry;
    cache->count++;
    return CACHE_OK;
}

const char* cache_get(const Cache* cache, const char* key) {
    const struct Entry* entry = find(cache, key);
    return entry == NULL ? NULL : entry->value;
}

size_t cache_size(const Cache* cache) { return cache->count; }
