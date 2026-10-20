/*
 * store.h - thread-safe key/value store (byte-string keys and values).
 *
 * The keyspace is split into STORE_SHARDS independent hash tables, each with
 * its own mutex. Threads only contend when they touch keys in the same shard,
 * which is what lets every core execute commands at the same time.
 *
 * Every function is safe to call from any thread. Multi-key commands (DEL,
 * EXISTS) call these once per key, so they are NOT atomic as a whole.
 */
#ifndef REDIS_MC_STORE_H
#define REDIS_MC_STORE_H

#include <stdbool.h>
#include <stddef.h>

#include "resp.h"   /* struct buf */

#define STORE_SHARDS 256   /* power of two */

struct store;

struct store *store_create(void);
void store_destroy(struct store *s);

/* If the key exists, append its value to *out and return true.
 * The value is copied while the shard is locked, so it is safe to use after
 * other threads change the key. */
bool store_get(struct store *s, const char *key, size_t klen, struct buf *out);

/* Insert or overwrite. */
void store_set(struct store *s, const char *key, size_t klen,
               const char *val, size_t vlen);

/* Remove the key. Returns true if it existed. */
bool store_del(struct store *s, const char *key, size_t klen);

bool store_exists(struct store *s, const char *key, size_t klen);

enum store_incr_result {
    STORE_INCR_OK,          /* *result holds the new value */
    STORE_INCR_NOT_INT,     /* stored value is not a canonical integer */
    STORE_INCR_OVERFLOW     /* value is already LLONG_MAX */
};

/* Add 1 to the integer stored at key (missing key counts as 0), atomically.
 * Value rules: docs/spec/commands.md section 6. On error the value is unchanged. */
enum store_incr_result store_incr(struct store *s, const char *key, size_t klen,
                                  long long *result);

#endif /* REDIS_MC_STORE_H */
