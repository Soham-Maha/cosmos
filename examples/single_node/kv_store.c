/* lseek and ftruncate are POSIX, and -std=c11 alone hides them behind a feature-test macro. */
#define _POSIX_C_SOURCE 200809L

#include "kv_store.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* On-disk record: magic, crc32, key_len, val_len, then the key bytes and the value bytes. Fields
   are in host byte order, like any on-disk format that never leaves the machine that wrote it. */
#define KV_MAGIC 0x4B565741u /* "KVWA" */
#define KV_HEADER_BYTES 12u
#define KV_MAX_RECORD (KV_HEADER_BYTES + KV_KEY_MAX + KV_VALUE_MAX)
#define KV_PATH_MAX 4096

struct kv_entry {
    char key[KV_KEY_MAX];
    char value[KV_VALUE_MAX];
};

struct kv_store {
    int fd;
    size_t count;
    struct kv_entry entries[KV_MAX_ENTRIES];
};

/* A reflected CRC-32 (the zlib polynomial), kept table-less: the records are tiny and a 1 KiB table
   is not worth reading past in an example. */
static uint32_t crc32_update(uint32_t crc, const unsigned char* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

/* The stored value is the complement, so a record's bytes are checked as ~crc32_update(...). */
static void store_u32(unsigned char* p, uint32_t value) { memcpy(p, &value, sizeof value); }
static void store_u16(unsigned char* p, uint16_t value) { memcpy(p, &value, sizeof value); }

static uint32_t load_u32(const unsigned char* p) {
    uint32_t value = 0;
    memcpy(&value, p, sizeof value);
    return value;
}

static uint16_t load_u16(const unsigned char* p) {
    uint16_t value = 0;
    memcpy(&value, p, sizeof value);
    return value;
}

/* Hand-rolled rather than strncpy so the truncation rule is visible: the destination always ends
   with a terminator, and a value that did not fit was already refused by the caller. */
static void copy_bounded(char* dst, size_t cap, const char* src) {
    size_t i = 0;
    while (i + 1 < cap && src[i] != '\0') {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static struct kv_entry* find_entry(struct kv_store* store, const char* key) {
    for (size_t i = 0; i < store->count; ++i) {
        if (strcmp(store->entries[i].key, key) == 0) {
            return &store->entries[i];
        }
    }
    return NULL;
}

/* Replaces an existing key in place, appends otherwise. A table already at its limit drops the new
   key rather than overwriting an older one, which is what replay wants: the log stays the truth for
   what was written, and the limit bounds what can be answered. */
static void table_store(struct kv_store* store, const char* key, const char* value) {
    struct kv_entry* entry = find_entry(store, key);
    if (entry == NULL) {
        if (store->count == KV_MAX_ENTRIES) {
            return;
        }
        entry = &store->entries[store->count++];
        copy_bounded(entry->key, KV_KEY_MAX, key);
    }
    copy_bounded(entry->value, KV_VALUE_MAX, value);
}

/* Bytes actually transferred: short only at end of file, -1 when the read itself failed. */
static ssize_t read_full(int fd, unsigned char* buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        const ssize_t got = read(fd, buf + done, len - done);
        if (got == 0) {
            break;
        }
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        done += (size_t)got;
    }
    return (ssize_t)done;
}

/* Replays the log from the start and leaves the descriptor where the next append belongs. Returns
   1 when the log is usable, 0 when it cannot be read at all. */
static int replay(struct kv_store* store) {
    unsigned char header[KV_HEADER_BYTES];
    unsigned char payload[KV_KEY_MAX + KV_VALUE_MAX];
    off_t valid_end = 0;
    int torn = 0;

    for (;;) {
        const ssize_t got = read_full(store->fd, header, KV_HEADER_BYTES);
        if (got < 0) {
            return 0;
        }
        if (got == 0) {
            break; /* clean end: every record in the file parsed */
        }
        if (got != (ssize_t)KV_HEADER_BYTES) {
            torn = 1; /* a torn write cut into the header */
            break;
        }

        const uint32_t magic = load_u32(header);
        const uint16_t key_len = load_u16(header + 8);
        const uint16_t value_len = load_u16(header + 10);
        /* The lengths are trusted only after the magic, and only as far as the fixed buffers: a
           zeroed sector must stop replay, not size an unwanted read. */
        if (magic != KV_MAGIC || key_len == 0 || key_len >= KV_KEY_MAX ||
            value_len >= KV_VALUE_MAX) {
            torn = 1;
            break;
        }

        const size_t payload_len = (size_t)key_len + (size_t)value_len;
        if (read_full(store->fd, payload, payload_len) != (ssize_t)payload_len) {
            torn = 1;
            break;
        }

        /* The crc covers the length fields and the payload, never the magic: replay recognises a
           damaged sector by the magic before it trusts what follows it. */
        uint32_t crc = crc32_update(0xFFFFFFFFu, header + 8, 4);
        crc = crc32_update(crc, payload, payload_len);
        if (load_u32(header + 4) != ~crc) {
            torn = 1;
            break;
        }

        char key[KV_KEY_MAX];
        char value[KV_VALUE_MAX];
        memcpy(key, payload, key_len);
        key[key_len] = '\0';
        memcpy(value, payload + key_len, value_len);
        value[value_len] = '\0';
        table_store(store, key, value);

        valid_end += (off_t)(KV_HEADER_BYTES + payload_len);
    }

    /* Cut a torn tail off rather than merely ignoring it: the next append then lands where the next
       replay will look, and a store that lost its tail can still accept new records. */
    if (torn && ftruncate(store->fd, valid_end) != 0) {
        return 0;
    }
    return lseek(store->fd, valid_end, SEEK_SET) >= 0;
}

kv_store* kv_open(const char* dir) {
    if (dir == NULL) {
        return NULL;
    }

    const size_t dir_len = strlen(dir);
    const size_t name_len = strlen(KV_LOG_NAME);
    if (dir_len + 1 + name_len + 1 > KV_PATH_MAX) {
        return NULL;
    }

    char path[KV_PATH_MAX];
    memcpy(path, dir, dir_len);
    path[dir_len] = '/';
    memcpy(path + dir_len + 1, KV_LOG_NAME, name_len + 1);

    /* No O_APPEND and no O_TRUNC: replay decides where the log ends, and a torn tail may have to be
       cut back before the descriptor is positioned there. */
    const int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return NULL;
    }

    struct kv_store* store = malloc(sizeof *store);
    if (store == NULL) {
        close(fd);
        return NULL;
    }
    store->fd = fd;
    store->count = 0;

    if (!replay(store)) {
        close(fd);
        free(store);
        return NULL;
    }
    return store;
}

int kv_put(kv_store* store, const char* key, const char* value, int durable) {
    if (store == NULL || key == NULL || value == NULL) {
        return KV_ERR;
    }

    const size_t key_len = strlen(key);
    const size_t value_len = strlen(value);
    if (key_len == 0 || key_len >= KV_KEY_MAX || value_len >= KV_VALUE_MAX) {
        return KV_ERR;
    }
    /* Refused before anything reaches the log: a record the table could never answer would make a
       later replay's view depend on insertion order. */
    if (find_entry(store, key) == NULL && store->count == KV_MAX_ENTRIES) {
        return KV_ERR;
    }

    unsigned char record[KV_MAX_RECORD];
    const size_t payload_len = key_len + value_len;
    store_u32(record, KV_MAGIC);
    store_u16(record + 8, (uint16_t)key_len);
    store_u16(record + 10, (uint16_t)value_len);
    memcpy(record + KV_HEADER_BYTES, key, key_len);
    memcpy(record + KV_HEADER_BYTES + key_len, value, value_len);
    uint32_t crc = crc32_update(0xFFFFFFFFu, record + 8, 4);
    store_u32(record + 4, ~crc32_update(crc, record + KV_HEADER_BYTES, payload_len));
    const size_t total = KV_HEADER_BYTES + payload_len;

    /* The log is append-only and the descriptor sits at end of file, so this is where the record
       will land. A failed put has to put the file back exactly there. */
    const off_t start = lseek(store->fd, 0, SEEK_CUR);
    if (start < 0) {
        return KV_ERR;
    }

    /* One write per record. A short return or a failed fsync means the record was not accepted, so
       its bytes are cut back: a partial record would make the next replay stop there and discard
       every record appended after it, including ones already acknowledged as durable. */
    ssize_t wrote;
    do {
        wrote = write(store->fd, record, total);
    } while (wrote < 0 && errno == EINTR);
    if (wrote < 0 || (size_t)wrote != total || (durable && fsync(store->fd) != 0)) {
        if (ftruncate(store->fd, start) == 0) {
            (void)lseek(store->fd, start, SEEK_SET);
        }
        return KV_ERR;
    }

    /* The acknowledgement: only now may the in-memory table claim the value, and only after the
       caller asked for the fsync did the claim outlive the process. */
    table_store(store, key, value);
    return KV_OK;
}

int kv_get(kv_store* store, const char* key, char* out, size_t cap) {
    if (store == NULL || key == NULL) {
        return KV_ERR;
    }
    const struct kv_entry* entry = find_entry(store, key);
    if (entry == NULL) {
        return KV_ERR;
    }
    const size_t value_len = strlen(entry->value);
    if (out == NULL || cap <= value_len) {
        return KV_ERR;
    }
    memcpy(out, entry->value, value_len + 1);
    return KV_OK;
}

void kv_close(kv_store* store) {
    if (store == NULL) {
        return;
    }
    /* Close only: a store that was never told to fsync has nothing more to promise its caller. */
    close(store->fd);
    free(store);
}

#ifndef COSMOS_KV_NO_MAIN
/* The standalone example: one durable put, read back through the same log. */
int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "kv_store_data";
    (void)mkdir(dir, 0700); /* EEXIST is the ordinary case: a previous run left its log behind */

    kv_store* store = kv_open(dir);
    if (store == NULL) {
        fprintf(stderr, "cannot open %s/%s\n", dir, KV_LOG_NAME);
        return 1;
    }

    char value[KV_VALUE_MAX];
    if (kv_put(store, "hello", "world", 1) != KV_OK ||
        kv_get(store, "hello", value, sizeof value) != KV_OK) {
        kv_close(store);
        return 1;
    }
    printf("%s/%s: hello = %s\n", dir, KV_LOG_NAME, value);
    kv_close(store);
    return 0;
}
#endif
