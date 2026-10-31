/* Experimental storage tool. Never linked into or selected by mc_server. */
#ifdef NDEBUG
#error "Fixed-work verification requires assertions"
#endif
#include "mc_region_txn.h"
#include "mc_anvil.h"
#include "mc_atomic_file.h"
#include "mc_nbt.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

static uint64_t clock_ns(clockid_t id) {
    struct timespec t;
    assert(clock_gettime(id, &t) == 0);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void failure(const char *op) {
    perror(op);
    exit(1);
}
static void put32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}
static long number(const char *s, long lo, long hi) {
    char *end;
    errno = 0;
    long n = strtol(s, &end, 10);
    if (errno || *end || end == s || n < lo || n > hi) {
        fprintf(stderr, "invalid number: %s\n", s);
        exit(2);
    }
    return n;
}
static void location(char path[4096], const char *root, unsigned region) {
    int n = snprintf(path, 4096, "%s/r.%d.-1.mca", root, -1 - (int)region);
    assert(n > 0 && n < 4096);
}
static int init(const char *root) {
    if (mkdir(root, 0700) != 0)
        return -1;
    if (mc_persistence_mkdirs(root, 0700) != 0)
        return -1;
    char marker[4096];
    int n = snprintf(marker, sizeof(marker), "%s/%s", root, MC_REGION_TXN_MARKER);
    if (n < 0 || n >= (int)sizeof(marker)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = open(marker, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    if (mc_file_write_all(fd, MC_REGION_TXN_MARKER_TEXT, strlen(MC_REGION_TXN_MARKER_TEXT)) != 0 ||
        fsync(fd) != 0 || close(fd) != 0)
        return -1;
    int dir = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0)
        return -1;
    int r = fsync(dir), e = errno;
    if (close(dir) != 0 && !r) {
        r = -1;
        e = errno;
    }
    errno = e;
    return r;
}
static uint8_t *nbt(unsigned value, size_t *n) {
    mc_nbt_tag_t version = {.type = MC_NBT_TAG_INT, .name = "DataVersion", .payload.int_val = 4788};
    mc_nbt_tag_t state = {.type = MC_NBT_TAG_INT, .name = "S2Value", .payload.int_val = (int32_t)value};
    mc_nbt_tag_t opaque = {
        .type = MC_NBT_TAG_STRING, .name = "Uninterpreted", .payload.string_val = "GoatCore S2 benchmark"};
    mc_nbt_tag_t *children[] = {&version, &state, &opaque};
    mc_nbt_tag_t root = {.type = MC_NBT_TAG_COMPOUND, .name = "", .payload.compound = {3, children}};
    uint8_t *out = NULL;
    assert(mc_nbt_write_named_root(&root, &out, n) == 0);
    return out;
}
static void fixture(const char *root, unsigned region, unsigned count) {
    char path[4096];
    location(path, root, region);
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    assert(fd >= 0);
    uint8_t header[8192] = {0};
    assert(ftruncate(fd, (off_t)(3 + count) * 4096) == 0);
    for (unsigned i = 0; i < count; i++) {
        size_t n;
        uint8_t *raw = nbt(i, &n), record[4096] = {0};
        uLongf len = sizeof(record) - 5;
        assert(compress2(record + 5, &len, raw, n, Z_DEFAULT_COMPRESSION) == Z_OK);
        free(raw);
        put32(record, (uint32_t)len + 1);
        record[4] = 2;
        put32(header + 4 * i, (2 + i) << 8 | 1);
        put32(header + 4096 + 4 * i, 123456 + i);
        assert(mc_file_pwrite_all(fd, record, sizeof(record), (off_t)(2 + i) * 4096) == 0);
    }
    assert(mc_file_pwrite_all(fd, header, sizeof(header), 0) == 0 && fsync(fd) == 0 && close(fd) == 0);
}
static void verify(const char *root, unsigned regions, unsigned count, const unsigned *values) {
    for (unsigned r = 0; r < regions; r++) {
        char path[4096];
        location(path, root, r);
        mc_region_file_t *f = NULL;
        assert(mc_region_open(path, &f) == 0);
        assert(mc_region_stats(mc_region_file_index(f))->chunks == count);
        for (unsigned i = 0; i < count; i++) {
            size_t actual_len = 0, want_len;
            uint8_t *actual = NULL, *want = nbt(values[r * count + i], &want_len);
            assert(mc_anvil_read_chunk_indexed(f, i, &actual, &actual_len) == 0);
            assert(actual_len == want_len && !memcmp(want, actual, want_len));
            free(actual);
            free(want);
        }
        assert(mc_region_close(f) == 0);
    }
}
static int bench(const char *root, const char *backend, unsigned count, const char *pattern, unsigned ops) {
    bool incremental = !strcmp(backend, "s2");
    if (!incremental && strcmp(backend, "atomic"))
        return 2;
    if (strcmp(pattern, "single") && strcmp(pattern, "spread") && strcmp(pattern, "regions"))
        return 2;
    if (init(root) != 0)
        failure("fresh disposable directory");
    unsigned regions = !strcmp(pattern, "regions") ? 4 : 1;
    unsigned *values = calloc(regions * count, sizeof(*values));
    assert(values);
    for (unsigned r = 0; r < regions; r++) {
        fixture(root, r, count);
        for (unsigned i = 0; i < count; i++)
            values[r * count + i] = i;
    }
    mc_region_txn_store_t *s = NULL;
    if (incremental) {
        assert(mc_region_txn_open(root, MC_REGION_TXN_DISPOSABLE, &s) == 0);
        /* Warm ownership, initial generation and allocation; excluded identically
         * from both timed backends (one initial save per region). */
    }
    for (unsigned r = 0; r < regions; r++) {
        char path[4096];
        location(path, root, r);
        size_t n;
        uint8_t *raw = nbt(0, &n);
        mc_region_update_t u = {0, raw, n};
        assert((incremental ? mc_region_txn_write(s, -1 - (int32_t)r, -1, &u, 1)
                            : mc_anvil_write_chunk_nbt(path, 0, 0, raw, n)) == 0);
        free(raw);
    }
    uint64_t serial_ns = 0;
    uint64_t begin = clock_ns(CLOCK_MONOTONIC), cpu = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
    printf("{\"phase\":\"begin\",\"us\":%" PRIu64 "}\n", begin / 1000);
    fflush(stdout);
    for (unsigned i = 0; i < ops; i++) {
        unsigned r = !strcmp(pattern, "regions") ? i % regions : 0;
        unsigned slot = !strcmp(pattern, "single") ? 0 : (i / regions) % count;
        uint64_t start = clock_ns(CLOCK_MONOTONIC);
        size_t n;
        uint8_t *raw = nbt(10000 + i, &n);
        serial_ns += clock_ns(CLOCK_MONOTONIC) - start;
        char path[4096];
        location(path, root, r);
        mc_region_update_t u = {slot, raw, n};
        uint64_t tick = clock_ns(CLOCK_MONOTONIC), tcpu = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
        int rc = incremental ? mc_region_txn_write(s, -1 - (int32_t)r, -1, &u, 1)
                             : mc_anvil_write_chunk_nbt(path, slot % 32, slot / 32, raw, n);
        uint64_t used_cpu = clock_ns(CLOCK_PROCESS_CPUTIME_ID) - tcpu,
                 used = clock_ns(CLOCK_MONOTONIC) - tick;
        if (rc) {
            fprintf(stderr, "save failed rc=%d errno=%d\n", rc, errno);
            exit(1);
        }
        free(raw);
        values[r * count + slot] = 10000 + i;
        printf("{\"action\":%u,\"wall_ns\":%" PRIu64 ",\"cpu_ns\":%" PRIu64 "}\n", i, used, used_cpu);
    }
    uint64_t finish = clock_ns(CLOCK_MONOTONIC), used_cpu = clock_ns(CLOCK_PROCESS_CPUTIME_ID) - cpu;
    printf("{\"phase\":\"end\",\"us\":%" PRIu64 ",\"duration_ns\":%" PRIu64 ",\"cpu_ns\":%" PRIu64
           ",\"serialize_ns\":%" PRIu64 ",\"confirmed\":%u}\n",
           finish / 1000, finish - begin, used_cpu, serial_ns, ops);
    fflush(stdout);
    if (s)
        assert(mc_region_txn_close(s) == 0);
    verify(root, regions, count, values);
    free(values);
    for (unsigned r = 0; r < regions; r++) {
        char path[4096];
        location(path, root, r);
        mc_region_file_t *f = NULL;
        assert(mc_region_open(path, &f) == 0);
        const mc_region_stats_t *st = mc_region_stats(mc_region_file_index(f));
        printf("{\"region\":%u,\"sectors\":%u,\"occupied\":%u,\"free\":%u}\n", r, st->sectors, st->occupied,
               st->free_sectors);
        assert(mc_region_close(f) == 0);
    }
    return 0;
}
/* Isolated S5 study. Both backends receive the SAME immutable NBT batch and
 * read the same expected source before each transaction. This is not a new
 * journal protocol or an integration into mc_server. */
static int bench_batched(const char *root, const char *backend, unsigned count,
                         unsigned width, unsigned ops) {
    bool incremental = !strcmp(backend, "s2");
    if ((!incremental && strcmp(backend, "atomic")) || width > count || ops > count)
        return 2;
    if (init(root) != 0)
        failure("fresh disposable directory");
    fixture(root, 0, count);
    char path[4096];
    location(path, root, 0);
    unsigned *values = calloc(count, sizeof(*values));
    assert(values);
    for (unsigned i = 0; i < count; i++)
        values[i] = i;
    mc_region_txn_store_t *store = NULL;
    if (incremental)
        assert(mc_region_txn_open(root, MC_REGION_TXN_DISPOSABLE, &store) == 0);
    size_t n;
    uint8_t *raw = nbt(0, &n);
    mc_region_update_t warm = {0, raw, n};
    assert((incremental ? mc_region_txn_write(store, -1, -1, &warm, 1)
                        : mc_anvil_write_chunk_nbt(path, 0, 0, raw, n)) == 0);
    free(raw);
    uint64_t begin = clock_ns(CLOCK_MONOTONIC), cpu = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
    printf("{\"phase\":\"begin\",\"us\":%" PRIu64 "}\n", begin / 1000);
    unsigned transactions = 0;
    for (unsigned first = 0; first < ops; first += width) {
        uint64_t start = clock_ns(CLOCK_MONOTONIC), tcpu = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
        unsigned amount = ops - first < width ? ops - first : width;
        mc_region_update_t updates[64];
        mc_anvil_chunk_write_t writes[64];
        uint8_t *before[64];
        mc_region_file_t *reader = NULL;
        assert(mc_region_open(path, &reader) == 0);
        for (unsigned j = 0; j < amount; j++) {
            size_t old_n;
            unsigned slot = first + j;
            assert(mc_anvil_read_chunk_indexed(reader, slot, &before[j], &old_n) == 0);
            uint8_t *next = nbt(10000 + slot, &n);
            updates[j] = (mc_region_update_t){slot, next, n};
            writes[j] = (mc_anvil_chunk_write_t){slot, next, n, before[j], old_n};
        }
        assert(mc_region_close(reader) == 0);
        int rc = incremental ? mc_region_txn_write(store, -1, -1, updates, amount)
                             : mc_anvil_write_batch_expected(path, writes, amount);
        if (rc)
            failure("batch publication");
        for (unsigned j = 0; j < amount; j++) {
            values[first + j] = 10000 + first + j;
            free(before[j]);
            free((void *)updates[j].nbt);
        }
        printf("{\"transaction\":%u,\"changes\":%u,\"wall_ns\":%" PRIu64 ",\"cpu_ns\":%" PRIu64 "}\n",
               transactions++, amount, clock_ns(CLOCK_MONOTONIC) - start,
               clock_ns(CLOCK_PROCESS_CPUTIME_ID) - tcpu);
    }
    uint64_t finish = clock_ns(CLOCK_MONOTONIC);
    printf("{\"phase\":\"end\",\"us\":%" PRIu64 ",\"duration_ns\":%" PRIu64
           ",\"cpu_ns\":%" PRIu64 ",\"confirmed\":%u,\"transactions\":%u,\"width\":%u}\n",
           finish / 1000, finish - begin, clock_ns(CLOCK_PROCESS_CPUTIME_ID) - cpu,
           ops, transactions, width);
    if (store)
        assert(mc_region_txn_close(store) == 0);
    verify(root, 1, count, values);
    free(values);
    puts("{\"readback\":true,\"all_chunks_and_opaque_fields_verified\":true}");
    return 0;
}
static int rewrite(const char *path, const char *xs, const char *zs) {
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path)
        return 2;
    const char *name = slash + 1, *start = name + 2;
    char *end;
    if (strncmp(name, "r.", 2))
        return 2;
    errno = 0;
    long rx = strtol(start, &end, 10);
    if (errno || end == start || *end != '.' || rx < INT32_MIN || rx > INT32_MAX)
        return 2;
    start = end + 1;
    errno = 0;
    long rz = strtol(start, &end, 10);
    if (errno || end == start || strcmp(end, ".mca") || rz < INT32_MIN || rz > INT32_MAX)
        return 2;
    int lx = (int)number(xs, 0, 31), lz = (int)number(zs, 0, 31);
    char *root = strndup(path, (size_t)(slash - path));
    assert(root);
    mc_region_txn_store_t *s = NULL;
    if (mc_region_txn_open(root, MC_REGION_TXN_DISPOSABLE, &s) != 0)
        failure("open/recover");
    uint8_t *raw = NULL;
    size_t n = 0;
    assert(mc_anvil_read_chunk_nbt(path, lx, lz, &raw, &n) == 0);
    mc_region_update_t u = {(unsigned)(lx + lz * 32), raw, n};
    int rc = mc_region_txn_write(s, (int32_t)rx, (int32_t)rz, &u, 1);
    free(raw);
    int close_rc = mc_region_txn_close(s);
    free(root);
    printf("{\"result\":%d,\"nbt_bytes\":%zu,\"experimental\":true}\n", rc, n);
    return rc || close_rc ? 1 : 0;
}
int main(int argc, char **argv) {
    if (argc < 4 || strcmp(argv[1], "--experimental-disposable")) {
        fprintf(stderr,
                "usage: %s --experimental-disposable init|recover DIR\n       ... rewrite-region PATH LX "
                "LZ\n       ... bench FRESH_DIR atomic|s2 CHUNKS single|spread|regions OPS\n"
                "       ... bench-batched FRESH_DIR atomic|s2 CHUNKS WIDTH OPS\n",
                argv[0]);
        return 2;
    }
    if (argc == 4 && !strcmp(argv[2], "init")) {
        if (init(argv[3]) != 0)
            failure("init");
        return 0;
    }
    if (argc == 4 && !strcmp(argv[2], "recover")) {
        uint64_t t = clock_ns(CLOCK_MONOTONIC), c = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
        mc_region_txn_store_t *s = NULL;
        if (mc_region_txn_open(argv[3], MC_REGION_TXN_DISPOSABLE, &s) != 0)
            failure("recovery blocked");
        if (mc_region_txn_close(s) != 0)
            failure("close");
        printf("{\"result\":0,\"recovery_ns\":%" PRIu64 ",\"cpu_ns\":%" PRIu64 "}\n",
               clock_ns(CLOCK_MONOTONIC) - t, clock_ns(CLOCK_PROCESS_CPUTIME_ID) - c);
        return 0;
    }
    if (argc == 6 && !strcmp(argv[2], "rewrite-region"))
        return rewrite(argv[3], argv[4], argv[5]);
    if (argc == 8 && !strcmp(argv[2], "bench"))
        return bench(argv[3], argv[4], (unsigned)number(argv[5], 1, 1024), argv[6],
                     (unsigned)number(argv[7], 1, 100000));
    if (argc == 8 && !strcmp(argv[2], "bench-batched"))
        return bench_batched(argv[3], argv[4], (unsigned)number(argv[5], 1, 1024),
                             (unsigned)number(argv[6], 1, 64), (unsigned)number(argv[7], 1, 1024));
    return 2;
}
