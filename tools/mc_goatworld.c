#define _GNU_SOURCE
#include "mc_goatworld.h"
#include "mc_anvil.h"
#include "mc_atomic_file.h"
#include "mc_nbt.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int32_t x, z, rx, rz;
    unsigned slot;
    uint8_t *raw;
    size_t length;
    uint64_t gen;
} item_t;
typedef struct {
    item_t *v;
    size_t n;
    uint64_t bytes;
} corpus_t;
static uint64_t now(clockid_t c) {
    struct timespec t;
    if (clock_gettime(c, &t))
        abort();
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
static unsigned long peak_rss(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f)
        return 0;
    char line[256];
    unsigned long peak = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long n;
        if (sscanf(line, "VmHWM: %lu kB", &n) == 1)
            peak = n;
    }
    if (fclose(f))
        return 0;
    return peak;
}
static uint64_t wall(void) { return now(CLOCK_MONOTONIC); }
static uint64_t cpu(void) { return now(CLOCK_PROCESS_CPUTIME_ID); }
static void fail(const char *where) {
    fprintf(stderr, "G1 failed: %s errno=%d (%s)\n", where, errno, strerror(errno));
    exit(1);
}
static void check(int rc, const char *where) {
    if (rc == G1_UNCERTAIN) {
        fprintf(stderr, "G1 durability uncertain after visible publication: %s\n", where);
        exit(3);
    }
    if (rc)
        fail(where);
}

static void independent(const char *source, const char *destination) {
    int rc = g1_disjoint(source, destination);
    if (rc == -4)
        exit(2);
    check(rc, "independent source and destination required");
}
static void corpus_digest(corpus_t *c, char out[65]) {
    uint8_t *data = calloc(c->n, 40);
    if (!data)
        fail("digest allocation");
    for (size_t i = 0; i < c->n; i++) {
        uint32_t v[2] = {(uint32_t)c->v[i].x, (uint32_t)c->v[i].z};
        for (unsigned k = 0; k < 2; k++)
            for (unsigned b = 0; b < 4; b++)
                data[i * 40 + k * 4 + b] = (uint8_t)(v[k] >> (24 - b * 8));
        check(g1_sha(c->v[i].raw, c->v[i].length, data + i * 40 + 8), "digest chunk");
    }
    uint8_t digest[32];
    check(g1_sha(data, c->n * 40, digest), "digest corpus");
    free(data);
    for (unsigned i = 0; i < 32; i++)
        snprintf(out + i * 2, 3, "%02x", digest[i]);
}
static long number(const char *text, long min, long max) {
    char *end;
    errno = 0;
    long n = strtol(text, &end, 10);
    if (errno || *end || n < min || n > max) {
        errno = EINVAL;
        fail("number");
    }
    return n;
}
static g1_backend_t backend(const char *s) {
    if (!strcmp(s, "native"))
        return G1_NATIVE;
    if (!strcmp(s, "sqlite"))
        return G1_SQLITE;
    if (!strcmp(s, "anvil"))
        return G1_ANVIL;
    errno = EINVAL;
    fail("backend");
    return 0;
}
static g1_profile_t profile(const char *s) {
    g1_profile_t p;
    check(g1_profile(s, &p), "profile");
    return p;
}
static int compare_item(const void *a, const void *b) {
    const item_t *x = a, *y = b;
    if (x->rx != y->rx)
        return x->rx < y->rx ? -1 : 1;
    if (x->rz != y->rz)
        return x->rz < y->rz ? -1 : 1;
    return x->slot < y->slot ? -1 : x->slot > y->slot;
}
static void free_corpus(corpus_t *c) {
    for (size_t i = 0; i < c->n; i++)
        free(c->v[i].raw);
    free(c->v);
    memset(c, 0, sizeof(*c));
}
static void add(corpus_t *c, int32_t x, int32_t z, uint8_t *raw, size_t n) {
    if (c->n >= 100000 || n > G1_MAX_RAW || c->bytes + n > 512u * 1024u * 1024u ||
        g1_validate_nbt(raw, n, x, z)) {
        errno = EINVAL;
        fail("corpus bounds/schema");
    }
    item_t *v = realloc(c->v, (c->n + 1) * sizeof(*v));
    if (!v)
        fail("corpus allocation");
    c->v = v;
    item_t *e = &c->v[c->n++];
    *e = (item_t){.x = x, .z = z, .raw = raw, .length = n, .gen = 1};
    mc_region_coordinates(x, z, &e->rx, &e->rz, &e->slot);
    c->bytes += n;
}
static corpus_t load(const char *path) {
    corpus_t c = {0};
    DIR *dir = opendir(path);
    if (!dir)
        fail("corpus directory");
    struct dirent *de;
    while ((de = readdir(dir))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        int32_t x, z;
        int chars = 0;
        char canonical[128];
        if (sscanf(de->d_name, "c_%d_%d.nbt%n", &x, &z, &chars) != 2 || !chars || de->d_name[chars]) {
            if (strstr(de->d_name, ".nbt"))
                fail("corpus name");
            continue;
        }
        snprintf(canonical, sizeof(canonical), "c_%d_%d.nbt", x, z);
        if (strcmp(canonical, de->d_name))
            fail("noncanonical corpus name");
        int fd = openat(dirfd(dir), de->d_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size < 1 ||
            st.st_size > G1_MAX_RAW)
            fail("corpus file");
        uint8_t *raw = malloc((size_t)st.st_size);
        if (!raw)
            fail("corpus allocation");
        check(mc_file_pread_all(fd, raw, (size_t)st.st_size, 0), "corpus read");
        check(close(fd), "corpus close");
        add(&c, x, z, raw, (size_t)st.st_size);
    }
    check(closedir(dir), "corpus closedir");
    qsort(c.v, c.n, sizeof(*c.v), compare_item);
    for (size_t i = 1; i < c.n; i++)
        if (!compare_item(&c.v[i - 1], &c.v[i]))
            fail("duplicate corpus coordinate");
    if (!c.n)
        fail("empty corpus");
    return c;
}
static void save_corpus(const char *path, corpus_t *c) {
    if (mkdir(path, 0700))
        fail("corpus destination must be absent");
    int dir = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0)
        fail("corpus destination");
    for (size_t i = 0; i < c->n; i++) {
        char name[128];
        snprintf(name, sizeof(name), "c_%d_%d.nbt", c->v[i].x, c->v[i].z);
        int fd = openat(dir, name, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0)
            fail("corpus create");
        check(mc_file_write_all(fd, c->v[i].raw, c->v[i].length), "corpus write");
        check(fsync(fd), "corpus sync");
        check(close(fd), "corpus close");
    }
    check(fsync(dir), "corpus directory sync");
    check(close(dir), "corpus dir close");
}
static corpus_t from_regions(const char *path) {
    corpus_t c = {0};
    DIR *dir = opendir(path);
    if (!dir)
        fail("region directory");
    struct dirent *de;
    while ((de = readdir(dir))) {
        int32_t rx, rz;
        int chars = 0;
        if (sscanf(de->d_name, "r.%d.%d.mca%n", &rx, &rz, &chars) != 2 || !chars || de->d_name[chars]) {
            if (strstr(de->d_name, ".mcc"))
                fail("external mcc unsupported");
            continue;
        }
        char canonical[128], full[4096];
        snprintf(canonical, sizeof(canonical), "r.%d.%d.mca", rx, rz);
        if (strcmp(canonical, de->d_name) || rx < INT32_MIN / 32 || rx > (INT32_MAX - 31) / 32 ||
            rz < INT32_MIN / 32 || rz > (INT32_MAX - 31) / 32)
            fail("region coordinates");
        if (snprintf(full, sizeof(full), "%s/%s", path, canonical) >= (int)sizeof(full))
            fail("region path");
        mc_region_file_t *f = NULL;
        check(mc_region_open(full, &f), "region open");
        const mc_region_stats_t *st = mc_region_stats(mc_region_file_index(f));
        if (st->external || st->unsupported)
            fail("unknown codec or external records");
        for (unsigned slot = 0; slot < 1024; slot++) {
            if (!mc_region_entry(mc_region_file_index(f), slot)->sectors)
                continue;
            uint8_t *raw = NULL;
            size_t n = 0;
            check(mc_anvil_read_chunk_indexed(f, slot, &raw, &n), "region inflate");
            add(&c, rx * 32 + (int32_t)(slot % 32), rz * 32 + (int32_t)(slot / 32), raw, n);
        }
        check(mc_region_close(f), "region close");
    }
    check(closedir(dir), "region closedir");
    qsort(c.v, c.n, sizeof(*c.v), compare_item);
    if (!c.n)
        fail("empty region corpus");
    return c;
}
static g1_store_t *open_store(const char *path, g1_backend_t b, bool create, bool write, size_t cache) {
    g1_store_t *s = NULL;
    check(g1_open(path, b, create, write, cache, &s), "store open");
    return s;
}
static void seed(g1_store_t *s, corpus_t *c, g1_profile_t p, size_t width) {
    for (size_t at = 0; at < c->n;) {
        g1_update_t u[64] = {0};
        size_t n = 0, total = 0;
        while (at + n < c->n && n < width && c->v[at + n].rx == c->v[at].rx &&
               c->v[at + n].rz == c->v[at].rz) {
            item_t *e = &c->v[at + n];
            if (n && total + e->length > G1_MAX_BATCH_BYTES)
                break;
            u[n] =
                (g1_update_t){.x = e->x, .z = e->z, .raw = e->raw, .length = e->length, .generation = e->gen};
            total += e->length;
            n++;
        }
        uint64_t a = wall(), b = cpu();
        check(g1_write(s, 0, u, n, p), "seed write");
        printf("{\"phase\":\"seed_transaction\",\"chunks\":%zu,\"wall_ns\":%" PRIu64 ",\"cpu_ns\":%" PRIu64
               "}\n",
               n, wall() - a, cpu() - b);
        at += n;
    }
}
static void compare_all(g1_store_t *s, corpus_t *c) {
    size_t n = 0;
    check(g1_verify(s, &n), "store verify");
    if (n != c->n)
        fail("verified count");
    for (size_t i = 0; i < c->n; i++) {
        uint8_t *raw = NULL;
        size_t len = 0;
        uint64_t gen;
        check(g1_read(s, 0, c->v[i].x, c->v[i].z, &raw, &len, &gen), "readback");
        if (len != c->v[i].length || memcmp(raw, c->v[i].raw, len) || (gen && gen != c->v[i].gen))
            fail("readback differs");
        free(raw);
    }
}
static void phase(const char *name, uint64_t w, uint64_t c, size_t count) {
    printf("{\"phase\":\"%s\",\"wall_ns\":%" PRIu64 ",\"cpu_ns\":%" PRIu64 ",\"chunks\":%zu}\n", name,
           wall() - w, cpu() - c, count);
}
static void allocation_walk(int root, uint64_t *logical, uint64_t *blocks, uint64_t *directory,
                            uint64_t *files) {
    DIR *dir = fdopendir(openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!dir)
        fail("allocation directory");
    struct dirent *de;
    while ((de = readdir(dir))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        struct stat st;
        if (fstatat(root, de->d_name, &st, AT_SYMLINK_NOFOLLOW))
            fail("allocation stat");
        if (S_ISDIR(st.st_mode)) {
            *directory += (uint64_t)st.st_blocks * 512;
            int next = openat(root, de->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (next < 0)
                fail("allocation child");
            allocation_walk(next, logical, blocks, directory, files);
            check(close(next), "allocation close");
        } else if (S_ISREG(st.st_mode)) {
            *logical += (uint64_t)st.st_size;
            *blocks += (uint64_t)st.st_blocks * 512;
            (*files)++;
        } else
            fail("allocation unsupported file");
    }
    check(closedir(dir), "allocation closedir");
}
static void allocation(const char *path, const char *stage) {
    int root = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root < 0)
        fail("allocation root");
    struct stat st;
    if (fstat(root, &st))
        fail("allocation root stat");
    uint64_t logical = 0, blocks = 0, dirs = (uint64_t)st.st_blocks * 512, files = 0;
    allocation_walk(root, &logical, &blocks, &dirs, &files);
    check(close(root), "allocation root close");
    printf("{\"phase\":\"allocation\",\"stage\":\"%s\",\"logical_bytes\":%" PRIu64
           ",\"allocated_file_bytes\":%" PRIu64 ",\"allocated_directory_bytes\":%" PRIu64
           ",\"files\":%" PRIu64 "}\n",
           stage, logical, blocks, dirs, files);
}
static void eviction_walk(int root, uint64_t *pages, uint64_t *resident) {
    DIR *dir = fdopendir(openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!dir)
        fail("eviction directory");
    struct dirent *de;
    while ((de = readdir(dir))) {
        if (de->d_name[0] == '.')
            continue;
        struct stat st;
        if (fstatat(root, de->d_name, &st, AT_SYMLINK_NOFOLLOW))
            fail("eviction stat");
        if (S_ISDIR(st.st_mode)) {
            int next = openat(root, de->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (next < 0)
                fail("eviction child");
            eviction_walk(next, pages, resident);
            check(close(next), "eviction close");
        } else if (S_ISREG(st.st_mode) && st.st_size) {
            int fd = openat(root, de->d_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (fd < 0)
                fail("eviction open");
            if (st.st_size > 1024LL * 1024 * 1024)
                fail("eviction bound");
            int r = posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
            if (r) {
                errno = r;
                fail("fadvise DONTNEED");
            }
            size_t len = (size_t)st.st_size, ps = (size_t)sysconf(_SC_PAGESIZE), n = (len + ps - 1) / ps;
            uint8_t *vec = malloc(n);
            void *map = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
            if (!vec || map == MAP_FAILED || mincore(map, len, vec))
                fail("mincore");
            for (size_t i = 0; i < n; i++)
                *resident += (vec[i] & 1) != 0;
            *pages += n;
            free(vec);
            check(munmap(map, len), "munmap");
            check(close(fd), "eviction file close");
        }
    }
    check(closedir(dir), "eviction closedir");
}
static void evict(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        fail("eviction root");
    uint64_t pages = 0, resident = 0;
    eviction_walk(fd, &pages, &resident);
    check(close(fd), "eviction root close");
    printf("{\"phase\":\"page_cache_residency_after_advice\",\"pages\":%" PRIu64 ",\"resident\":%" PRIu64
           ",\"cold_pages_confirmed\":%s,\"device_cache_cold_claim\":false}\n",
           pages, resident, resident ? "false" : "true");
}
static void read_pattern(g1_store_t *s, corpus_t *c, const char *pattern, size_t count) {
    struct sample {
        size_t at, len;
        uint64_t w, u;
    };
    struct sample *samples = calloc(count, sizeof(*samples));
    if (!samples)
        fail("read samples");
    uint32_t random = 7754788;
    uint64_t start = wall(), started_cpu = cpu();
    for (size_t i = 0; i < count; i++) {
        size_t at = i % c->n;
        if (!strcmp(pattern, "random-warm")) {
            random = random * 1664525u + 1013904223u;
            at = random % c->n;
        } else if (!strcmp(pattern, "neighbors-warm") || !strcmp(pattern, "neighbors-prefetch"))
            at = ((i / 16) * 37 + i % 16) % c->n;
        else if (!strcmp(pattern, "application-cache"))
            at = i % (c->n < 32 ? c->n : 32);
        item_t *e = &c->v[at];
        if (!strcmp(pattern, "neighbors-prefetch"))
            check(g1_prefetch(s, 0, e->x, e->z), "prefetch");
        uint8_t *raw = NULL;
        size_t len = 0;
        uint64_t gen;
        uint64_t w = wall(), u = cpu();
        check(g1_read(s, 0, e->x, e->z, &raw, &len, &gen), "pattern read");
        samples[i] = (struct sample){at, len, wall() - w, cpu() - u};
        if (len != e->length || memcmp(raw, e->raw, len))
            fail("pattern raw mismatch");
        free(raw);
    }
    uint64_t total = wall() - start, used = cpu() - started_cpu;
    for (size_t i = 0; i < count; i++) {
        item_t *e = &c->v[samples[i].at];
        printf("{\"phase\":\"read\",\"pattern\":\"%s\",\"i\":%zu,\"x\":%d,\"z\":%d,\"wall_ns\":%" PRIu64
               ",\"cpu_ns\":%" PRIu64 ",\"bytes\":%zu}\n",
               pattern, i, e->x, e->z, samples[i].w, samples[i].u, samples[i].len);
    }
    free(samples);
    printf("{\"phase\":\"%s\",\"wall_ns\":%" PRIu64 ",\"cpu_ns\":%" PRIu64 ",\"chunks\":%zu}\n", pattern,
           total, used, count);
}
static uint8_t *revision(item_t *e, uint64_t value, size_t *n) {
    mc_nbt_tag_t *root = NULL;
    size_t used = 0;
    check(mc_nbt_read_named_root(e->raw, e->length, &root, &used), "revision parse");
    if (used != e->length)
        fail("revision trailing");
    mc_nbt_tag_t *tag = (mc_nbt_tag_t *)mc_nbt_compound_get(root, "G1BenchRevision");
    if (!tag) {
        tag = calloc(1, sizeof(*tag));
        if (!tag)
            fail("revision tag");
        tag->type = MC_NBT_TAG_LONG;
        tag->name = strdup("G1BenchRevision");
        mc_nbt_tag_t **children = realloc(root->payload.compound.children,
                                          ((size_t)root->payload.compound.length + 1) * sizeof(*children));
        if (!children || !tag->name)
            fail("revision children");
        root->payload.compound.children = children;
        root->payload.compound.children[root->payload.compound.length++] = tag;
    }
    if (tag->type != MC_NBT_TAG_LONG)
        fail("reserved G1BenchRevision collision");
    tag->payload.long_val = (int64_t)value;
    uint8_t *raw = NULL;
    check(mc_nbt_write_named_root(root, &raw, n), "revision encode");
    mc_nbt_free(root);
    return raw;
}
static void changes(g1_store_t *s, corpus_t *c, g1_profile_t p, size_t width, size_t mutations, bool same) {
    size_t at = 0, tx = 0;
    uint64_t start = wall(), started_cpu = cpu();
    while (at < mutations) {
        g1_update_t u[64] = {0};
        uint8_t *raw[64] = {0};
        size_t indices[64], count = 0, total = 0;
        size_t first = same ? 0 : at % c->n;
        while (at + count < mutations && count < (same ? 1 : width)) {
            size_t index = same ? 0 : (at + count) % c->n;
            item_t *e = &c->v[index];
            if (e->rx != c->v[first].rx || e->rz != c->v[first].rz)
                break;
            bool duplicate = false;
            for (size_t j = 0; j < count; j++)
                if (indices[j] == index)
                    duplicate = true;
            if (duplicate)
                break;
            size_t n = 0;
            raw[count] = revision(e, e->gen + 1, &n);
            if (count && total + n > G1_MAX_BATCH_BYTES) {
                free(raw[count]);
                raw[count] = NULL;
                break;
            }
            u[count] =
                (g1_update_t){.x = e->x, .z = e->z, .raw = raw[count], .length = n, .generation = e->gen + 1};
            check(g1_sha(e->raw, e->length, u[count].expected), "snapshot hash");
            indices[count] = index;
            total += n;
            count++;
        }
        uint64_t w = wall(), cpu_start = cpu();
        check(g1_write(s, 0, u, count, p), "mutation commit");
        uint64_t elapsed = wall() - w, used = cpu() - cpu_start;
        for (size_t i = 0; i < count; i++) {
            item_t *e = &c->v[indices[i]];
            free(e->raw);
            e->raw = raw[i];
            e->length = u[i].length;
            e->gen = u[i].generation;
        }
        printf(
            "{\"phase\":\"write\",\"pattern\":\"%s\",\"transaction\":%zu,\"updates\":%zu,\"wall_ns\":%" PRIu64
            ",\"cpu_ns\":%" PRIu64 "}\n",
            same ? "same-isolated" : "spread-batched", tx++, count, elapsed, used);
        at += count;
    }
    phase(same ? "same-isolated" : "spread-batched", start, started_cpu, mutations);
}
static void bench(char **args) {
    independent(args[0], args[1]);
    corpus_t c = load(args[0]);
    const char *root = args[1];
    g1_backend_t b = backend(args[2]);
    g1_profile_t p = profile(args[3]);
    size_t width = (size_t)number(args[4], 1, 64), mutations = (size_t)number(args[5], 1, 10000),
           reads = (size_t)number(args[6], 1, 100000);
    uint64_t all = wall(), all_cpu = cpu(), w = wall(), u = cpu();
    g1_store_t *s = open_store(root, b, true, true, 0);
    phase("create", w, u, c.n);
    w = wall();
    u = cpu();
    seed(s, &c, p, width);
    phase("seed", w, u, c.n);
    allocation(root, "seed-open");
    w = wall();
    u = cpu();
    check(g1_close(s), "seed close/checkpoint");
    phase("seed-close", w, u, c.n);
    allocation(root, "seed-closed");
    evict(root);
    w = wall();
    u = cpu();
    s = open_store(root, b, false, false, 0);
    phase("open-index-empty", w, u, c.n);
    read_pattern(s, &c, "sequential-after-advice", c.n);
    read_pattern(s, &c, "random-warm", reads);
    read_pattern(s, &c, "neighbors-warm", reads);
    read_pattern(s, &c, "neighbors-prefetch", reads);
    check(g1_close(s), "reader close");
    s = open_store(root, b, false, false, 8u * 1024u * 1024u);
    read_pattern(s, &c, "application-cache", reads);
    g1_cache_stats_t stats;
    g1_cache_stats(s, &stats);
    printf("{\"phase\":\"cache\",\"hits\":%" PRIu64 ",\"misses\":%" PRIu64 ",\"bytes\":%" PRIu64
           ",\"limit\":%" PRIu64 "}\n",
           stats.hits, stats.misses, stats.bytes, stats.limit);
    check(g1_close(s), "cache close");
    s = open_store(root, b, false, true, 0);
    changes(s, &c, p, width, mutations, true);
    changes(s, &c, p, width, mutations, false);
    allocation(root, "modified-open");
    w = wall();
    u = cpu();
    check(g1_close(s), "modified close/checkpoint");
    phase("modified-close", w, u, c.n);
    allocation(root, "modified-closed");
    w = wall();
    u = cpu();
    s = open_store(root, b, false, true, 0);
    phase("recovery-open", w, u, c.n);
    compare_all(s, &c);
    w = wall();
    u = cpu();
    check(g1_compact(s), "compact");
    phase("compact", w, u, c.n);
    check(g1_close(s), "compact close");
    allocation(root, "compact-closed");
    s = open_store(root, b, false, false, 0);
    compare_all(s, &c);
    check(g1_close(s), "final close");
    char signature[65];
    corpus_digest(&c, signature);
    printf("{\"phase\":\"complete\",\"chunks\":%zu,\"confirmed_mutations\":%zu,\"all_payloads_verified\":"
           "true,\"payload_sha256\":\"%s\",\"wall_ns\":%" PRIu64 ",\"cpu_ns\":%" PRIu64 ",\"rss_kb\":%ld}\n",
           c.n, mutations * 2, signature, wall() - all, cpu() - all_cpu, (long)peak_rss());
    free_corpus(&c);
}
static void codec_bench(char **args) {
    corpus_t c = load(args[0]);
    size_t reps = (size_t)number(args[1], 1, 100);
    const char *names[] = {"none",  "zlib1",   "zlib6",     "lz4",       "zstd1",    "zstd3",
                           "zstd9", "lz4-16k", "zstd3-16k", "none-pack", "lz4-pack", "zstd3-pack"};
    for (size_t p = 0; p < sizeof(names) / sizeof(*names); p++) {
        g1_profile_t profile_value = profile(names[p]);
        uint64_t encoded_total = 0, enc_cpu = 0, dec_cpu = 0, enc_wall = 0, dec_wall = 0;
        for (size_t rep = 0; rep < reps; rep++)
            for (size_t i = 0; i < c.n; i++) {
                uint8_t *encoded = NULL, *decoded = NULL;
                size_t n = 0;
                uint64_t w = wall(), u = cpu();
                check(g1_encode(c.v[i].raw, c.v[i].length, profile_value, &encoded, &n), "codec encode");
                enc_wall += wall() - w;
                enc_cpu += cpu() - u;
                w = wall();
                u = cpu();
                check(g1_decode(encoded, n, c.v[i].length, profile_value, &decoded), "codec decode");
                dec_wall += wall() - w;
                dec_cpu += cpu() - u;
                if (memcmp(decoded, c.v[i].raw, c.v[i].length))
                    fail("codec exact roundtrip");
                encoded_total += n;
                free(encoded);
                free(decoded);
            }
        printf("{\"profile\":\"%s\",\"chunks\":%zu,\"repetitions\":%zu,\"raw_bytes\":%" PRIu64
               ",\"encoded_bytes\":%" PRIu64 ",\"compress_cpu_ns\":%" PRIu64 ",\"decompress_cpu_ns\":%" PRIu64
               ",\"compress_wall_ns\":%" PRIu64 ",\"decompress_wall_ns\":%" PRIu64 ",\"exact\":true}\n",
               names[p], c.n, reps, c.bytes * reps, encoded_total, enc_cpu, dec_cpu, enc_wall, dec_wall);
    }
    free_corpus(&c);
}
int main(int argc, char **argv) {
    if (argc < 3 || strcmp(argv[1], "--experimental-disposable")) {
        fprintf(stderr, "G1 experimental disposable prototype only\n");
        return 2;
    }
    char **a = argv + 3;
    int n = argc - 3;
    if (!strcmp(argv[2], "versions") && n == 0) {
        puts(g1_codec_versions());
        return 0;
    }
    if (!strcmp(argv[2], "unpack") && n == 2) {
        independent(a[0], a[1]);
        corpus_t c = from_regions(a[0]);
        save_corpus(a[1], &c);
        printf("{\"unpacked\":%zu,\"source_not_written\":true}\n", c.n);
        free_corpus(&c);
        return 0;
    }
    if (!strcmp(argv[2], "import") && n == 5) {
        independent(a[0], a[1]);
        corpus_t c = load(a[0]);
        g1_store_t *s = open_store(a[1], backend(a[2]), true, true, 0);
        seed(s, &c, profile(a[3]), (size_t)number(a[4], 1, 64));
        compare_all(s, &c);
        check(g1_close(s), "import close");
        printf("{\"imported\":%zu,\"verified\":true}\n", c.n);
        free_corpus(&c);
        return 0;
    }
    if (!strcmp(argv[2], "verify") && n == 2) {
        g1_store_t *s = open_store(a[0], backend(a[1]), false, false, 0);
        size_t count = 0;
        check(g1_verify(s, &count), "verify");
        check(g1_close(s), "verify close");
        printf("{\"verified\":%zu}\n", count);
        return 0;
    }
    if (!strcmp(argv[2], "compact") && n == 2) {
        g1_store_t *s = open_store(a[0], backend(a[1]), false, true, 0);
        check(g1_compact(s), "compact");
        check(g1_close(s), "compact close");
        puts("{\"compacted\":true}");
        return 0;
    }
    if (!strcmp(argv[2], "mutate") && n == 7) {
        corpus_t c = load(a[2]);
        g1_store_t *s = open_store(a[0], backend(a[1]), false, true, 0);
        for (size_t i = 0; i < c.n; i++) {
            uint8_t *raw = NULL;
            size_t len = 0;
            uint64_t gen = 0;
            check(g1_read(s, 0, c.v[i].x, c.v[i].z, &raw, &len, &gen), "capture current generation");
            if (len != c.v[i].length || memcmp(raw, c.v[i].raw, len)) {
                free(raw);
                errno = ESTALE;
                fail("stale mutation source");
            }
            if (gen)
                c.v[i].gen = gen;
            else {
                mc_nbt_tag_t *root = NULL;
                size_t used = 0;
                check(mc_nbt_read_named_root(raw, len, &root, &used), "generation NBT");
                const mc_nbt_tag_t *t = mc_nbt_compound_get(root, "G1BenchRevision");
                if (t && t->type == MC_NBT_TAG_LONG && t->payload.long_val > 0)
                    c.v[i].gen = (uint64_t)t->payload.long_val;
                mc_nbt_free(root);
            }
            free(raw);
        }
        changes(s, &c, profile(a[3]), (size_t)number(a[4], 1, 64), (size_t)number(a[5], 1, 10000),
                !strcmp(a[6], "same"));
        check(g1_close(s), "mutate close");
        free_corpus(&c);
        return 0;
    }
    if (!strcmp(argv[2], "bench") && n == 7) {
        bench(a);
        return 0;
    }
    if (!strcmp(argv[2], "codec") && n == 2) {
        codec_bench(a);
        return 0;
    }
    if (!strcmp(argv[2], "export") && n == 3) {
        independent(a[0], a[2]);
        g1_store_t *s = open_store(a[0], backend(a[1]), false, false, 0);
        uint32_t *d = NULL;
        int32_t *x = NULL, *z = NULL;
        size_t count = 0;
        check(g1_keys(s, &d, &x, &z, &count), "export keys");
        corpus_t c = {0};
        for (size_t i = 0; i < count; i++) {
            if (d[i]) {
                errno = EOPNOTSUPP;
                fail("only overworld export tested");
            }
            uint8_t *raw = NULL;
            size_t len = 0;
            uint64_t gen;
            check(g1_read(s, 0, x[i], z[i], &raw, &len, &gen), "export read");
            add(&c, x[i], z[i], raw, len);
        }
        free(d);
        free(x);
        free(z);
        check(g1_close(s), "export store close");
        qsort(c.v, c.n, sizeof(*c.v), compare_item);
        /* Private absent directory. No successful receipt until all regions re-read. */
        if (mkdir(a[2], 0700))
            fail("export destination must be absent");
        for (size_t first = 0; first < c.n;) {
            g1_update_t unused = {0};
            (void)unused;
            mc_anvil_chunk_write_t batch[64];
            size_t end = first;
            while (end < c.n && end - first < 64 && c.v[end].rx == c.v[first].rx &&
                   c.v[end].rz == c.v[first].rz) {
                batch[end - first] =
                    (mc_anvil_chunk_write_t){c.v[end].slot, c.v[end].raw, c.v[end].length, NULL, 0};
                end++;
            }
            char path[4096];
            if (snprintf(path, sizeof(path), "%s/r.%d.%d.mca", a[2], c.v[first].rx, c.v[first].rz) >=
                (int)sizeof(path))
                fail("export region path");
            check(mc_anvil_write_batch_expected(path, batch, end - first), "export region durable");
            first = end;
        }
        corpus_t exported = from_regions(a[2]);
        if (exported.n != c.n)
            fail("export count");
        for (size_t i = 0; i < c.n; i++)
            if (c.v[i].x != exported.v[i].x || c.v[i].z != exported.v[i].z ||
                c.v[i].length != exported.v[i].length || memcmp(c.v[i].raw, exported.v[i].raw, c.v[i].length))
                fail("export data differs");
        printf("{\"exported\":%zu,\"exact_nbt\":true,\"chunk_export_only\":true}\n", c.n);
        free_corpus(&c);
        free_corpus(&exported);
        return 0;
    }
    fprintf(stderr, "usage: mc_goatworld --experimental-disposable versions|unpack REGIONS CORPUS|import "
                    "CORPUS ROOT native|sqlite|anvil PROFILE WIDTH|verify ROOT BACKEND|compact ROOT "
                    "BACKEND|export ROOT BACKEND REGIONS|bench CORPUS ROOT BACKEND PROFILE WIDTH CHANGES "
                    "READS|codec CORPUS REPS|mutate ROOT BACKEND CORPUS PROFILE WIDTH CHANGES same|spread\n");
    return 2;
}
