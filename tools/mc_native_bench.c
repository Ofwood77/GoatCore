/* S3B test-only probe. Requires a disposable-world marker, never used by mc_server. */
#include "mc_world_storage.h"
#include "mc_anvil.h"
#include "mc_atomic_file.h"
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
int main(int argc, char **argv) {
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
    if (mc_storage_open(world, MC_STORAGE_ANVIL_ATOMIC, &store) != 0)
        fail("storage open");
    if (seed) {
        int fd = open(argv[5], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 1 ||
            st.st_size > 64 * 1024 * 1024)
            fail("seed NBT");
        uint8_t *raw = malloc((size_t)st.st_size);
        if (!raw || mc_file_pread_all(fd, raw, (size_t)st.st_size, 0) != 0 || close(fd) != 0)
            fail("seed read");
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
    if (getrusage(RUSAGE_SELF, &u) != 0) fail("rusage");
    /* ru_maxrss can retain the launching Python process's pre-exec peak.
     * /proc's VmHWM belongs to this executable's current address space. */
    unsigned long rss=0,hwm=0;char line[256];FILE *status=fopen("/proc/self/status","r");
    if(!status)fail("proc status");
    while(fgets(line,sizeof(line),status)) {
        unsigned long n;
        if(sscanf(line,"VmRSS: %lu kB",&n)==1)rss=n;
        if(sscanf(line,"VmHWM: %lu kB",&n)==1)hwm=n;
    }
    if(fclose(status)!=0 || !rss || !hwm)fail("proc memory");
    printf("{\"rss_kb\":%lu,\"rss_current_kb\":%lu,\"inherited_ru_maxrss_kb\":%ld,\"saved\":%d,\"readback\":true}\n",hwm,rss,u.ru_maxrss,reps);
    mc_chunk_destroy(&verify);
    mc_chunk_destroy(&c);
    mc_storage_close(store);
    return 0;
}
