/*
 * store.c - sharded, mutex-protected hash tables. Interface in store.h.
 */
#include "store.h"

#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#define SHARD_BITS 8
_Static_assert(STORE_SHARDS == (1 << SHARD_BITS), "STORE_SHARDS must be 2^SHARD_BITS");

#define INITIAL_BUCKETS 16     /* per shard, power of two */
#define CACHE_LINE      64

/* One key/value pair. The key is stored inline after the struct (one
 * allocation); the value is separate because SET can change its size. */
struct entry {
    struct entry *next;        /* next entry in the same bucket */
    uint64_t      hash;        /* full hash, kept so resizing needs no rehash */
    char         *val;
    size_t        vlen;
    size_t        klen;
    char          key[];       /* flexible array member: klen bytes */
};

/* Aligned to a cache line so two cores locking neighbouring shards do not
 * fight over the same line of memory (false sharing). */
struct shard {
    _Alignas(CACHE_LINE) pthread_mutex_t lock;
    struct entry **buckets;
    size_t         nbuckets;   /* power of two */
    size_t         count;
};

struct store {
    uint64_t     seed;
    struct shard shards[STORE_SHARDS];
};

/* ===========================================================================
 * Allocation
 * ======================================================================== */

static void die_oom(void)
{
    fputs("redis-mc: out of memory\n", stderr);
    abort();
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);   /* malloc(0) may return NULL */
    if (p == NULL)
        die_oom();
    return p;
}

static void *xcalloc(size_t count, size_t size)
{
    void *p = calloc(count, size);
    if (p == NULL)
        die_oom();
    return p;
}

static char *dup_bytes(const char *p, size_t n)
{
    char *d = xmalloc(n);
    if (n > 0)
        memcpy(d, p, n);
    return d;
}

/* ===========================================================================
 * Hashing
 * ======================================================================== */

/* FNV-1a over the key, seeded per process, then a final mix so that both the
 * high bits (shard) and the low bits (bucket) are well spread. */
static uint64_t hash_key(const struct store *s, const char *key, size_t klen)
{
    uint64_t h = 14695981039346656037ULL ^ s->seed;
    for (size_t i = 0; i < klen; i++) {
        h ^= (unsigned char)key[i];
        h *= 1099511628211ULL;
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

static struct shard *shard_for(struct store *s, uint64_t h)
{
    return &s->shards[h >> (64 - SHARD_BITS)];   /* top bits pick the shard */
}

static size_t bucket_for(const struct shard *sh, uint64_t h)
{
    return (size_t)(h & (sh->nbuckets - 1));      /* low bits pick the bucket */
}

/* ===========================================================================
 * Shard internals (caller holds sh->lock)
 * ======================================================================== */

/* Returns the link that points at the matching entry (so it can be unlinked),
 * or the NULL link at the end of the chain if there is no match. */
static struct entry **find_link(struct shard *sh, uint64_t h,
                                const char *key, size_t klen)
{
    struct entry **link = &sh->buckets[bucket_for(sh, h)];
    while (*link != NULL) {
        struct entry *e = *link;
        if (e->hash == h && e->klen == klen && memcmp(e->key, key, klen) == 0)
            return link;
        link = &e->next;
    }
    return link;
}

static void grow(struct shard *sh)
{
    size_t n = sh->nbuckets * 2;
    struct entry **nb = xcalloc(n, sizeof *nb);
    for (size_t i = 0; i < sh->nbuckets; i++) {
        struct entry *e = sh->buckets[i];
        while (e != NULL) {
            struct entry *next = e->next;
            size_t b = (size_t)(e->hash & (n - 1));
            e->next = nb[b];
            nb[b] = e;
            e = next;
        }
    }
    free(sh->buckets);
    sh->buckets = nb;
    sh->nbuckets = n;
}

static void insert(struct shard *sh, struct entry *e)
{
    size_t b = bucket_for(sh, e->hash);
    e->next = sh->buckets[b];
    sh->buckets[b] = e;
    if (++sh->count > sh->nbuckets)   /* keep chains about 1 entry long */
        grow(sh);
}

static struct entry *new_entry(uint64_t h, const char *key, size_t klen,
                               char *val, size_t vlen)
{
    if (klen > SIZE_MAX - sizeof(struct entry))
        die_oom();
    struct entry *e = xmalloc(sizeof *e + klen);
    e->next = NULL;
    e->hash = h;
    e->val = val;
    e->vlen = vlen;
    e->klen = klen;
    if (klen > 0)
        memcpy(e->key, key, klen);
    return e;
}

static void free_entry(struct entry *e)
{
    free(e->val);
    free(e);
}

/* ===========================================================================
 * Public API
 * ======================================================================== */

struct store *store_create(void)
{
    /* aligned_alloc: the shards need 64-byte alignment, plain malloc only
     * guarantees 16. sizeof(struct store) is a multiple of 64 because of the
     * aligned member, as aligned_alloc requires. */
    struct store *s = aligned_alloc(CACHE_LINE, sizeof *s);
    if (s == NULL)
        die_oom();

    if (getrandom(&s->seed, sizeof s->seed, 0) != (ssize_t)sizeof s->seed)
        s->seed = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32);

    for (size_t i = 0; i < STORE_SHARDS; i++) {
        struct shard *sh = &s->shards[i];
        pthread_mutex_init(&sh->lock, NULL);
        sh->nbuckets = INITIAL_BUCKETS;
        sh->buckets = xcalloc(sh->nbuckets, sizeof *sh->buckets);
        sh->count = 0;
    }
    return s;
}

void store_destroy(struct store *s)
{
    if (s == NULL)
        return;
    for (size_t i = 0; i < STORE_SHARDS; i++) {
        struct shard *sh = &s->shards[i];
        for (size_t b = 0; b < sh->nbuckets; b++) {
            struct entry *e = sh->buckets[b];
            while (e != NULL) {
                struct entry *next = e->next;
                free_entry(e);
                e = next;
            }
        }
        free(sh->buckets);
        pthread_mutex_destroy(&sh->lock);
    }
    free(s);
}

bool store_get(struct store *s, const char *key, size_t klen, struct buf *out)
{
    uint64_t h = hash_key(s, key, klen);
    struct shard *sh = shard_for(s, h);

    pthread_mutex_lock(&sh->lock);
    struct entry *e = *find_link(sh, h, key, klen);
    if (e != NULL)
        buf_append(out, e->val, e->vlen);
    pthread_mutex_unlock(&sh->lock);

    return e != NULL;   /* only compares the pointer; e is not dereferenced */
}

void store_set(struct store *s, const char *key, size_t klen,
               const char *val, size_t vlen)
{
    uint64_t h = hash_key(s, key, klen);
    struct shard *sh = shard_for(s, h);

    /* All allocation and copying happens before taking the lock. */
    struct entry *fresh = new_entry(h, key, klen, dup_bytes(val, vlen), vlen);

    pthread_mutex_lock(&sh->lock);
    struct entry *old = *find_link(sh, h, key, klen);
    if (old != NULL) {
        /* Swap values: `old` keeps its place with the new value, `fresh`
         * now carries the previous value and is freed after unlocking. */
        char  *pv = old->val;
        size_t pl = old->vlen;
        old->val = fresh->val;
        old->vlen = fresh->vlen;
        fresh->val = pv;
        fresh->vlen = pl;
    } else {
        insert(sh, fresh);
        fresh = NULL;
    }
    pthread_mutex_unlock(&sh->lock);

    if (fresh != NULL)
        free_entry(fresh);
}

bool store_del(struct store *s, const char *key, size_t klen)
{
    uint64_t h = hash_key(s, key, klen);
    struct shard *sh = shard_for(s, h);

    pthread_mutex_lock(&sh->lock);
    struct entry **link = find_link(sh, h, key, klen);
    struct entry *e = *link;
    if (e != NULL) {
        *link = e->next;
        sh->count--;
    }
    pthread_mutex_unlock(&sh->lock);

    if (e == NULL)
        return false;
    free_entry(e);      /* freeing outside the lock keeps the lock short */
    return true;
}

bool store_exists(struct store *s, const char *key, size_t klen)
{
    uint64_t h = hash_key(s, key, klen);
    struct shard *sh = shard_for(s, h);

    pthread_mutex_lock(&sh->lock);
    bool found = *find_link(sh, h, key, klen) != NULL;
    pthread_mutex_unlock(&sh->lock);
    return found;
}

/* Canonical 64-bit decimal (docs/spec/commands.md section 6): optional '-', no
 * leading zeros, no '+', no spaces, no "-0", within [LLONG_MIN, LLONG_MAX]. */
static bool parse_canonical_ll(const char *p, size_t n, long long *out)
{
    if (n == 0 || n > 20)
        return false;

    size_t i = 0;
    bool neg = p[0] == '-';
    if (neg)
        i = 1;
    if (i == n)
        return false;                       /* just "-" */
    if (p[i] == '0') {
        if (neg || n != 1)
            return false;                   /* "-0", "01", "-01" */
        *out = 0;
        return true;
    }

    unsigned long long u = 0;
    for (; i < n; i++) {
        if (p[i] < '0' || p[i] > '9')
            return false;
        unsigned d = (unsigned)(p[i] - '0');
        if (u > (ULLONG_MAX - d) / 10)
            return false;
        u = u * 10 + d;
    }

    if (neg) {
        if (u > (unsigned long long)LLONG_MAX + 1)
            return false;
        /* -(LLONG_MAX + 1) cannot be written as a negation without overflow. */
        *out = (u == (unsigned long long)LLONG_MAX + 1) ? LLONG_MIN : -(long long)u;
    } else {
        if (u > (unsigned long long)LLONG_MAX)
            return false;
        *out = (long long)u;
    }
    return true;
}

enum store_incr_result store_incr(struct store *s, const char *key, size_t klen,
                                  long long *result)
{
    uint64_t h = hash_key(s, key, klen);
    struct shard *sh = shard_for(s, h);
    enum store_incr_result rc = STORE_INCR_OK;
    char tmp[32];

    /* Read, add and write back under one lock: that is what makes INCR
     * atomic when many threads increment the same key. */
    pthread_mutex_lock(&sh->lock);
    struct entry *e = *find_link(sh, h, key, klen);
    long long v = 0;

    if (e != NULL && !parse_canonical_ll(e->val, e->vlen, &v)) {
        rc = STORE_INCR_NOT_INT;
    } else if (v == LLONG_MAX) {
        rc = STORE_INCR_OVERFLOW;
    } else {
        v++;
        int n = snprintf(tmp, sizeof tmp, "%lld", v);
        char *nv = dup_bytes(tmp, (size_t)n);
        if (e != NULL) {
            free(e->val);
            e->val = nv;
            e->vlen = (size_t)n;
        } else {
            insert(sh, new_entry(h, key, klen, nv, (size_t)n));
        }
        *result = v;
    }
    pthread_mutex_unlock(&sh->lock);
    return rc;
}
