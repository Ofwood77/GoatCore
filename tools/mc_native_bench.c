/* S3B test-only probe. Requires a disposable-world marker, never used by mc_server. */
#include "mc_world_storage.h"
#include "mc_anvil.h"
#include "mc_atomic_file.h"
#ifndef MC_NATIVE_BENCH_BASELINE
#include "mc_save_pipeline.h"
#endif
#include <sys/resource.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
static uint64_t ns(clockid_t c) {
    struct timespec t;
    if (clock_gettime(c, &t))
        abort();
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void sample(const char *phase, unsigned i, uint64_t w, uint64_t cpu, size_t bytes) {
    printf("{\"phase\":\"%s\",\"i\":%u,\"ns\":%llu,\"cpu_ns\":%llu,\"bytes\":%zu}\n", phase, i,
           (unsigned long long)(ns(CLOCK_MONOTONIC) - w),
           (unsigned long long)(ns(CLOCK_THREAD_CPUTIME_ID) - cpu), bytes);
}
static void fail(const char *p) {
    perror(p);
    exit(1);
}
static int number(const char *s) {
    char *end;
    long n = strtol(s, &end, 10);
    if (*end || n < -1000000 || n > 1000000)
        exit(2);
    return (int)n;
}
static mc_world_storage_t *disposable(const char *world) {
    char marker[4096];
    struct stat st;
    if (snprintf(marker, sizeof(marker), "%s/.goatcore-s3b-disposable", world) >= (int)sizeof(marker) ||
        lstat(marker, &st) != 0 || !S_ISREG(st.st_mode))
        fail("disposable marker");
    mc_world_storage_t *s = NULL;
    mc_storage_kind_t kind=MC_STORAGE_ANVIL_ATOMIC;
    const char *test_backend=getenv("MC_TEST_STORAGE");
    if(test_backend && mc_storage_parse(test_backend,&kind))fail("test storage mode");
    if (mc_storage_open(world, kind, &s) != 0)
        fail("storage open");
    return s;
}
static void rss(unsigned changes, unsigned saved, unsigned unique, unsigned readback) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f)
        fail("status");
    char line[256];
    unsigned long peak = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long n;
        if (sscanf(line, "VmHWM: %lu kB", &n) == 1)
            peak = n;
    }
    if (fclose(f) != 0 || !peak)
        fail("memory");
    printf("{\"changes\":%u,\"saved_chunks\":%u,\"unique_dirty_chunks\":%u,\"readback_chunks\":%u,\"rss_kb\":%lu,\"readback\":true}\n", changes, saved, unique, readback, peak);
}
static int region_seed(char **argv) {
#ifdef MC_NATIVE_BENCH_BASELINE
    (void)argv;
    return 2;
#else
    mc_world_storage_t *s = disposable(argv[2]);
    int cx = number(argv[3]), cz = number(argv[4]), count = number(argv[5]);
    if (count < 0 || count > 1024 || cx % 32 || cz % 32)
        return 2;
    if (!count) { mc_storage_close(s); return 0; }
    mc_chunk_t *c = calloc((size_t)count, sizeof(*c));
    mc_chunk_t **p = calloc((size_t)count, sizeof(*p));
    if (!c || !p)
        fail("seed allocation");
    for (int i = 0; i < count; i++) {
        if (mc_chunk_init(&c[i], cx + i % 32, cz + i / 32, 0) != 0)
            fail("seed init");
        p[i] = &c[i];
    }
    if (mc_storage_write_region(s, p, (size_t)count) != 0)
        fail("seed region");
    for (int i = 0; i < count; i++)
        mc_chunk_destroy(&c[i]);
    free(c);
    free(p);
    mc_storage_close(s);
    return 0;
#endif
}
/* Same logical mutations, final durability boundary and immutable start world.
 * Serial mode publishes intermediate states; grouped mode coalesces them before
 * snapshot/flush. Neither mode claims those intermediate states are durable. */
static int batch_bench(char **argv) {
    int cx = number(argv[3]), cz = number(argv[4]), count = number(argv[5]), changes = number(argv[6]);
    bool serial = !strcmp(argv[8], "serial"), regions = !strcmp(argv[7], "regions");
    if (count < 1 || count > 1024 || changes < 1 || changes > 10000 ||
        (!serial && strcmp(argv[8], "grouped")) ||
        (strcmp(argv[7], "same") && strcmp(argv[7], "spread") && strcmp(argv[7], "absent") && !regions))
        return 2;
#ifdef MC_NATIVE_BENCH_BASELINE
    if (!serial)
        return 2;
#endif
    mc_world_storage_t *s = disposable(argv[2]);
    mc_chunk_t *live = calloc((size_t)count, sizeof(*live));
    bool *dirty = calloc((size_t)count, sizeof(*dirty));
    if (!live || !dirty)
        fail("live chunks");
    uint64_t load = ns(CLOCK_MONOTONIC), cpu = ns(CLOCK_THREAD_CPUTIME_ID);
    for (int i = 0; i < count; i++) {
        int x = cx + (regions ? i * 32 : i % 32), z = cz + (regions ? 0 : i / 32);
        int rc=mc_storage_read(s,x,z,&live[i],NULL);
        if (!strcmp(argv[7],"absent")) {
            if(rc!=1 || count!=1 || mc_chunk_init(&live[i],x,z,0)!=0)fail("expected absent chunk");
        } else if (rc != 0)
            fail("load bench chunk");
    }
    sample("load_all", 0, load, cpu, (size_t)count);
    uint64_t start = ns(CLOCK_MONOTONIC), start_cpu = ns(CLOCK_THREAD_CPUTIME_ID);
    uint32_t stone = mc_global_state_id_from_key("minecraft:stone", UINT32_MAX),
             dirt = mc_global_state_id_from_key("minecraft:dirt", UINT32_MAX);
    unsigned saved = 0;
    for (int i = 0; i < changes; i++) {
        int at = !strcmp(argv[7], "same") ? 0 : i % count;
        uint32_t old = mc_chunk_get_block(&live[at], 8, 200, 8);
        if (mc_chunk_set_block(&live[at], 8, 200, 8, old == stone ? dirt : stone) != 0)
            fail("logical mutation");
        dirty[at] = true;
        if (serial) {
            uint64_t w = ns(CLOCK_MONOTONIC), c = ns(CLOCK_THREAD_CPUTIME_ID);
            if (mc_storage_write(s, &live[at], NULL) != 0)
                fail("serial save");
            sample("save", (unsigned)i, w, c, live[at].storage_nbt_len);
            saved++;
        }
    }
#ifndef MC_NATIVE_BENCH_BASELINE
    if (!serial) {
        mc_save_batch_t *batch = calloc(1, sizeof(*batch));
        if (!batch)
            fail("batch allocation");
        for (int i = 0; i < count; i++)
            if (dirty[i]) {
                if (mc_save_batch_add(batch, &live[i]) != 0)
                    fail("snapshot");
                if (batch->count == MC_SAVE_BATCH_CHUNKS || i == count - 1 ||
                    (!strcmp(argv[7], "same") && i == 0)) {
                    uint64_t w = ns(CLOCK_MONOTONIC), c = ns(CLOCK_THREAD_CPUTIME_ID);
                    mc_save_batch_execute(s, batch);
                    for (size_t j = 0; j < batch->count; j++)
                        if (batch->results[j])
                            fail("grouped save");
                    sample("save", saved, w, c, batch->bytes);
                    saved += (unsigned)batch->count;
                    mc_save_batch_destroy(batch);
                    batch = calloc(1, sizeof(*batch));
                    if (!batch)
                        fail("next batch");
                }
            }
        /* The final dirty index may precede count-1 (e.g. 100 of 1024 chunks). */
        if (batch->count) {
            uint64_t w = ns(CLOCK_MONOTONIC), c = ns(CLOCK_THREAD_CPUTIME_ID);
            mc_save_batch_execute(s, batch);
            for (size_t j = 0; j < batch->count; j++)
                if (batch->results[j])
                    fail("grouped tail");
            sample("save", saved, w, c, batch->bytes);
            saved += (unsigned)batch->count;
        }
        mc_save_batch_destroy(batch);
    }
#endif
    sample("work_total", 0, start, start_cpu, (size_t)changes);
    for (int i = 0; i < count; i++) {
        mc_chunk_t verify;
        if (mc_storage_read(s, live[i].cx, live[i].cz, &verify, NULL) != 0 ||
            mc_chunk_get_block(&verify, 8, 200, 8) != mc_chunk_get_block(&live[i], 8, 200, 8))
            fail("full readback");
        mc_chunk_destroy(&verify);
        mc_chunk_destroy(&live[i]);
    }
    unsigned unique=0;for(int i=0;i<count;i++)unique+=dirty[i];
    rss((unsigned)changes, saved, unique, (unsigned)count);
    free(live);
    free(dirty);
    mc_storage_close(s);
    return 0;
}
int main(int argc, char **argv) {
    if (argc == 6 && !strcmp(argv[1], "--seed-region"))
        return region_seed(argv);
    if (argc == 9 && !strcmp(argv[1], "--batch-bench"))
        return batch_bench(argv);
    bool seed = argc == 6 && !strcmp(argv[1], "--seed");
    if (!seed && argc != 8) {
        fprintf(stderr, "usage: %s WORLD CX CZ LOCAL_X Y LOCAL_Z REPS | --seed WORLD CX CZ RAW_NBT\n",
                argv[0]);
        return 2;
    }
    const char *world = argv[seed ? 2 : 1];
    char path[4096];
    snprintf(path, sizeof(path), "%s/.goatcore-s3b-disposable", world);
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode))
        fail("disposable marker");
    int cx = number(argv[seed ? 3 : 2]), cz = number(argv[seed ? 4 : 3]);
    mc_world_storage_t *store = NULL;
    mc_storage_kind_t kind=MC_STORAGE_ANVIL_ATOMIC;
    const char *backend=getenv("MC_TEST_STORAGE");
    if(backend&&mc_storage_parse(backend,&kind))fail("test backend");
    if (mc_storage_open(world, kind, &store) != 0)
        fail("storage open");
    if (seed) {
        int fd = open(argv[5], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 1 ||
            st.st_size > 64 * 1024 * 1024)
            fail("seed NBT");
        uint8_t *raw = malloc((size_t)st.st_size);
        if (!raw || mc_file_pread_all(fd, raw, (size_t)st.st_size, 0) != 0 || close(fd) != 0)
            fail("seed read");
        if(kind==MC_STORAGE_GOATWORLD_SQLITE){
            mc_chunk_t chunk={0};
            if(mc_anvil_decode_native(raw,(size_t)st.st_size,cx,cz,&chunk))fail("test seed decode");
            chunk.storage_nbt=raw;chunk.storage_nbt_len=(size_t)st.st_size;
            if(!chunk.save_generation)chunk.save_generation=1;
            if(mc_storage_write(store,&chunk,NULL))fail("test seed SQLite");
            mc_chunk_destroy(&chunk);
            if(mc_storage_close_checked(store))fail("test seed close");
            return 0;
        }
        int32_t rx, rz;
        unsigned slot;
        mc_region_coordinates(cx, cz, &rx, &rz, &slot);
        snprintf(path, sizeof(path), "%s/dimensions/minecraft/overworld/region/r.%d.%d.mca", world, rx, rz);
        if (mc_anvil_write_chunk_nbt(path, slot % 32, slot / 32, raw, (size_t)st.st_size) != 0)
            fail("seed replace");
        free(raw);
        mc_storage_close(store);
        return 0;
    }
    int x = number(argv[4]), y = number(argv[5]), z = number(argv[6]), reps = number(argv[7]);
    if (x < 0 || x > 15 || z < 0 || z > 15 || y < -64 || y >= 320 || reps < 1 || reps > 10000)
        return 2;
    for (unsigned i = 0; i < 10; i++) {
        mc_chunk_t c;
        uint64_t w = ns(CLOCK_MONOTONIC), cpu = ns(CLOCK_THREAD_CPUTIME_ID);
        if (mc_storage_read(store, cx, cz, &c, NULL) != 0)
            fail("load");
        sample("load", i, w, cpu, c.storage_nbt_len);
        mc_chunk_destroy(&c);
    }
    mc_chunk_t c;
    if (mc_storage_read(store, cx, cz, &c, NULL) != 0)
        fail("load main");
    uint32_t stone = mc_global_state_id_from_key("minecraft:stone", UINT32_MAX),
             dirt = mc_global_state_id_from_key("minecraft:dirt", UINT32_MAX);
    for (unsigned i = 0; i < (unsigned)reps; i++) {
        if (mc_chunk_set_block(&c, x, y, z, i % 2 ? stone : dirt) != 0)
            fail("mutate");
        uint8_t *raw = NULL;
        size_t n = 0;
        uint64_t w = ns(CLOCK_MONOTONIC), cpu = ns(CLOCK_THREAD_CPUTIME_ID);
        if (mc_anvil_encode_native(&c, NULL, &raw, &n) != 0)
            fail("encode");
        sample("encode", i, w, cpu, n);
        free(raw);
        w = ns(CLOCK_MONOTONIC);
        cpu = ns(CLOCK_THREAD_CPUTIME_ID);
        if (mc_storage_write(store, &c, NULL) != 0)
            fail("save");
        sample("save", i, w, cpu, c.storage_nbt_len);
    }
    mc_chunk_t verify;
    if (mc_storage_read(store, cx, cz, &verify, NULL) != 0 ||
        mc_chunk_get_block(&verify, x, y, z) != mc_chunk_get_block(&c, x, y, z))
        fail("readback");
    struct rusage u;
    if (getrusage(RUSAGE_SELF, &u) != 0)
        fail("rusage");
    /* ru_maxrss can retain the launching Python process's pre-exec peak.
     * /proc's VmHWM belongs to this executable's current address space. */
    unsigned long rss = 0, hwm = 0;
    char line[256];
    FILE *status = fopen("/proc/self/status", "r");
    if (!status)
        fail("proc status");
    while (fgets(line, sizeof(line), status)) {
        unsigned long n;
        if (sscanf(line, "VmRSS: %lu kB", &n) == 1)
            rss = n;
        if (sscanf(line, "VmHWM: %lu kB", &n) == 1)
            hwm = n;
    }
    if (fclose(status) != 0 || !rss || !hwm)
        fail("proc memory");
    printf("{\"rss_kb\":%lu,\"rss_current_kb\":%lu,\"inherited_ru_maxrss_kb\":%ld,\"saved\":%d,\"readback\":"
           "true}\n",
           hwm, rss, u.ru_maxrss, reps);
    mc_chunk_destroy(&verify);
    mc_chunk_destroy(&c);
    if(mc_storage_close_checked(store))fail("storage close");
    return 0;
}
