#ifndef MC_GOATWORLD_H
#define MC_GOATWORLD_H
/* G1 disposable prototype ONLY. Not linked into mc_server. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define G1_MAX_RAW (8u * 1024u * 1024u)
#define G1_MAX_BATCH 64u
#define G1_MAX_BATCH_BYTES (32u * 1024u * 1024u)
#define G1_UNCERTAIN (-2)
typedef struct g1_store g1_store_t;
typedef enum { G1_NATIVE, G1_SQLITE, G1_ANVIL } g1_backend_t;
typedef struct {
    uint8_t codec, framed;
    int8_t level;
} g1_profile_t;
typedef struct {
    int32_t x, z;
    const uint8_t *raw;
    size_t length;
    uint64_t generation;
    uint8_t expected[32]; /* zero means absent; SHA-256 of captured previous raw NBT otherwise */
} g1_update_t;
typedef struct {
    uint64_t hits, misses, bytes, limit, reads, writes;
} g1_cache_stats_t;
/* Creates ONLY an absent path. Existing path requires correct G1 marker and backend. */
/* Offline tools: source/destination must be independent; -4 forbids report output. */
int g1_disjoint(const char *source, const char *destination);
int g1_open(const char *path, g1_backend_t backend, bool create, bool writable, size_t cache_limit,
            g1_store_t **out);
int g1_close(g1_store_t *store); /* SQLite includes a checked durable checkpoint */
int g1_read(g1_store_t *store, uint32_t dimension, int32_t x, int32_t z, uint8_t **raw, size_t *length,
            uint64_t *generation);
/* One region per transaction for ALL backends. Immutable inputs, caller-owned generation, CAS hashes. */
int g1_write(g1_store_t *store, uint32_t dimension, const g1_update_t *updates, size_t count,
             g1_profile_t profile);
int g1_compact(g1_store_t *store); /* offline, exclusive; each region individually durable */
int g1_verify(g1_store_t *store, size_t *chunks);
int g1_keys(g1_store_t *store, uint32_t **dimensions, int32_t **xs, int32_t **zs, size_t *count);
void g1_cache_stats(g1_store_t *store, g1_cache_stats_t *out);
int g1_prefetch(g1_store_t *store, uint32_t dimension, int32_t x, int32_t z); /* optional advisory only */
int g1_sha(const void *data, size_t length, uint8_t digest[32]);
int g1_encode(const uint8_t *raw, size_t length, g1_profile_t p, uint8_t **out, size_t *bytes);
int g1_decode(const uint8_t *data, size_t bytes, size_t raw_length, g1_profile_t p, uint8_t **out);
int g1_profile(const char *name, g1_profile_t *out);
const char *g1_codec_versions(void);
/* Exact full 4788 on-disk NBT envelope, coordinates/status, no normalization. */
int g1_validate_nbt(const uint8_t *data, size_t length, int32_t x, int32_t z);
#endif
