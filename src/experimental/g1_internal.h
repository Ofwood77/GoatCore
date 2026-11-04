#ifndef G1_INTERNAL_H
#define G1_INTERNAL_H
#include "mc_goatworld.h"
#include <sqlite3.h>
#include <pthread.h>
#include "mc_region_storage.h"
#define G1_INDEX_BYTES (128u + 1024u * 32u)
#define G1_CACHE_ENTRIES 64
#define G1_REGIONS 8
#define G1_RECORD_HEADER 96u
#define G1_DATA_HEADER 128u
struct g1_cache {
    uint8_t *data;
    size_t length;
    uint32_t dim;
    int32_t x, z;
    uint64_t gen, used;
};
struct g1_region {
    bool present;
    uint32_t dim;
    int32_t rx, rz;
    int fd;
    uint64_t used;
    uint8_t *index;
    mc_region_file_t *anvil;
};
struct g1_store {
    pthread_t owner;
    int root, lock;
    char path[4096];
    uint8_t id[16];
    g1_backend_t backend;
    bool writable, poisoned;
    sqlite3 *sql;
    sqlite3_stmt *read_sql;
    bool runtime;
    size_t cache_limit, cache_bytes;
    uint64_t clock, hits, misses, reads, writes;
    struct g1_cache cache[G1_CACHE_ENTRIES];
    struct g1_region regions[G1_REGIONS];
};
uint32_t g1_u32(const uint8_t *p);
uint64_t g1_u64(const uint8_t *p);
void g1_p32(uint8_t *p, uint32_t n);
void g1_p64(uint8_t *p, uint64_t n);
int g1_sync(int fd);
int g1_path(g1_store_t *s, const char *name, char path[4096]);
int g1_pack(const uint8_t *raw, size_t length, bool inverse, uint8_t **out, size_t *bytes);
int g1_sql_open(g1_store_t *s, bool create);
int g1_sql_read(g1_store_t *s, uint32_t dim, int32_t x, int32_t z, uint8_t **raw, size_t *length,
                uint64_t *gen);
int g1_sql_write(g1_store_t *s, uint32_t dim, const g1_update_t *u, size_t n, g1_profile_t p);
int g1_sql_keys(g1_store_t *s, uint32_t **d, int32_t **x, int32_t **z, size_t *n);
int g1_sql_finish(g1_store_t *s, bool compact);
int g1_sql_check(g1_store_t *s);
int g1_sql_close(g1_store_t *s);
#ifdef MC_GOATWORLD_TESTING
void g1_test_hook(const char *phase);
#define G1_HOOK(p) g1_test_hook(p)
#else
#define G1_HOOK(p) ((void)0)
#endif
#endif
