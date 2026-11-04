#define _GNU_SOURCE
#include "mc_goatworld.h"
#include "mc_anvil.h"
#include "block_registry.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

static unsigned checks;
#define CHECK(v)                                                                                             \
    do {                                                                                                     \
        checks++;                                                                                            \
        if (!(v)) {                                                                                          \
            fprintf(stderr, "G1 check line %d: %s errno=%d\n", __LINE__, #v, errno);                         \
            abort();                                                                                         \
        }                                                                                                    \
    } while (0)
static const char *profiles[] = {"none",  "zlib1",   "zlib6",     "lz4",       "zstd1",    "zstd3",
                                 "zstd9", "lz4-16k", "zstd3-16k", "none-pack", "lz4-pack", "zstd3-pack"};
static uint8_t *chunk(int32_t x, int32_t z, unsigned diversity, size_t *len) {
    mc_chunk_t c = {0};
    CHECK(!mc_chunk_init(&c, x, z, 0));
    uint32_t ids[4096];
    unsigned n = 0;
    for (uint32_t id = 1; id < GLOBAL_BLOCK_STATES_COUNT && n < diversity; id++)
        if (!mc_native_state_sensitive(id))
            ids[n++] = id;
    CHECK(n == diversity);
    for (unsigned i = 0; i < 4096; i++)
        CHECK(!mc_chunk_set_block(&c, i & 15, 64 + (int)(i >> 8), (i >> 4) & 15, ids[i % diversity]));
    uint8_t *raw = NULL;
    CHECK(!mc_anvil_encode_native(&c, NULL, &raw, len));
    mc_chunk_destroy(&c);
    CHECK(!g1_validate_nbt(raw, *len, x, z));
    return raw;
}
static void codecs(void) {
    unsigned values[] = {1, 2, 3, 16, 17, 32, 257, 4096};
    for (unsigned k = 0; k < sizeof(values) / sizeof(*values); k++) {
        size_t n = 0;
        uint8_t *raw = chunk(-33, 32, values[k], &n);
        for (unsigned j = 0; j < sizeof(profiles) / sizeof(*profiles); j++) {
            g1_profile_t p;
            CHECK(!g1_profile(profiles[j], &p));
            uint8_t *packed = NULL, *out = NULL;
            size_t bytes = 0;
            CHECK(!g1_encode(raw, n, p, &packed, &bytes));
            CHECK(!g1_decode(packed, bytes, n, p, &out));
            CHECK(!memcmp(raw, out, n));
            free(out);
            out = NULL;
            CHECK(g1_decode(packed, bytes - 1, n, p, &out) < 0);
            CHECK(!out);
            free(packed);
        }
        free(raw);
    }
    g1_profile_t bad = {99, 0, 0};
    uint8_t *out = NULL;
    size_t bytes = 0;
    CHECK(g1_encode((const uint8_t *)"x", 1, bad, &out, &bytes) < 0);
    CHECK(!out);
    /* Generic framing fallback preserves opaque bytes that are not a chunk tree. */
    size_t n = 70000;
    uint8_t *noise = malloc(n);
    CHECK(noise);
    uint32_t r = 7754788;
    for (size_t i = 0; i < n; i++) {
        r = r * 1664525u + 1013904223u;
        noise[i] = (uint8_t)(r >> 24);
    }
    for (unsigned j = 0; j < 9; j++) {
        g1_profile_t p;
        CHECK(!g1_profile(profiles[j], &p));
        uint8_t *encoded = NULL, *decoded = NULL;
        size_t len = 0;
        CHECK(!g1_encode(noise, n, p, &encoded, &len));
        CHECK(!g1_decode(encoded, len, n, p, &decoded));
        CHECK(!memcmp(noise, decoded, n));
        free(encoded);
        free(decoded);
    }
    free(noise);
}
static void *foreign_thread(void *pointer) {
    g1_store_t *s = pointer;
    uint8_t *raw = NULL;
    size_t n = 0;
    uint64_t gen = 0;
    int rc = g1_read(s, 0, 0, 0, &raw, &n, &gen);
    return (void *)(uintptr_t)(rc < 0 && errno == EPERM && !raw && n == 0);
}
static void storage(g1_backend_t b) {
    char lab[] = "/tmp/goatcore-g1-unit.XXXXXX";
    CHECK(mkdtemp(lab));
    char root[512];
    CHECK(snprintf(root, sizeof(root), "%s/store", lab) > 0);
    g1_store_t *s = NULL, *other = NULL;
    g1_profile_t p;
    CHECK(!g1_profile(b == G1_ANVIL ? "zlib6" : "zstd3-pack", &p));
    CHECK(!g1_open(root, b, true, true, 1024 * 1024, &s));
    CHECK(g1_open(root, b, true, true, 0, &other) < 0 && !other);
    CHECK(g1_open(root, b, false, true, 0, &other) < 0 && !other);
    CHECK(g1_open(root, b, false, false, 0, &other) < 0 && !other);
    pthread_t thread;
    void *result = NULL;
    CHECK(!pthread_create(&thread, NULL, foreign_thread, s));
    CHECK(!pthread_join(thread, &result));
    CHECK((uintptr_t)result == 1);
    int32_t x[] = {-33, -32, -1, 0, 32}, z[] = {32, 32, 0, 0, -1};
    uint8_t *raw[5] = {0};
    size_t len[5] = {0};
    for (unsigned i = 0; i < 5; i++) {
        raw[i] = chunk(x[i], z[i], i + 1, &len[i]);
        g1_update_t u = {.x = x[i], .z = z[i], .raw = raw[i], .length = len[i], .generation = 1};
        CHECK(!g1_write(s, 0, &u, 1, p));
    }
    for (unsigned i = 0; i < 5; i++) {
        uint8_t *out = NULL;
        size_t n = 0;
        uint64_t gen;
        CHECK(!g1_read(s, 0, x[i], z[i], &out, &n, &gen));
        CHECK(n == len[i] && !memcmp(out, raw[i], n));
        CHECK(gen == 1 || b == G1_ANVIL);
        free(out);
    }
    uint8_t *out = NULL;
    size_t n = 0;
    uint64_t gen = 0;
    CHECK(!g1_read(s, 0, x[0], z[0], &out, &n, &gen));
    free(out);
    g1_cache_stats_t cache;
    g1_cache_stats(s, &cache);
    CHECK(cache.hits >= 1 && cache.bytes <= cache.limit);
    size_t changed_n;
    uint8_t *changed = chunk(x[0], z[0], 19, &changed_n);
    g1_update_t u = {.x = x[0], .z = z[0], .raw = changed, .length = changed_n, .generation = 2};
    /* A stale captured snapshot is rejected before any backend write. */
    CHECK(g1_write(s, 0, &u, 1, p) < 0 && errno == ESTALE);
    CHECK(!g1_sha(raw[0], len[0], u.expected));
    CHECK(!g1_write(s, 0, &u, 1, p));
    CHECK(!g1_read(s, 0, x[0], z[0], &out, &n, &gen));
    CHECK(n == changed_n && !memcmp(out, changed, n));
    free(out);
    free(raw[0]);
    raw[0] = changed;
    len[0] = changed_n;
    /* Duplicate slots and cross-region transactions cannot partly mutate a batch. */
    CHECK(!g1_sha(changed, changed_n, u.expected));
    u.generation = 3;
    g1_update_t bad[2] = {u, u};
    CHECK(g1_write(s, 0, bad, 2, p) < 0);
    bad[1].x = x[4];
    bad[1].z = z[4];
    CHECK(g1_write(s, 0, bad, 2, p) < 0);
    CHECK(!g1_close(s));
    s = NULL;
    CHECK(!g1_open(root, b, false, true, 0, &s));
    size_t count = 0;
    CHECK(!g1_verify(s, &count) && count == 5);
    CHECK(!g1_compact(s));
    CHECK(!g1_close(s));
    CHECK(!g1_open(root, b, false, false, 0, &s));
    CHECK(!g1_verify(s, &count) && count == 5);
    for (unsigned i = 0; i < 5; i++) {
        CHECK(!g1_read(s, 0, x[i], z[i], &out, &n, &gen));
        CHECK(n == len[i] && !memcmp(out, raw[i], n));
        free(out);
        free(raw[i]);
    }
    CHECK(!g1_close(s));
    /* Cleanup is delegated to the runner; never issue shell deletion from C. */
    printf("G1 disposable fixture %s\n", lab);
}
int main(void) {
    codecs();
    storage(G1_NATIVE);
    storage(G1_SQLITE);
    storage(G1_ANVIL);
    printf("goatworld: %u checks passed\n", checks);
    return 0;
}
