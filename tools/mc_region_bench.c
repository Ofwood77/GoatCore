#include "mc_anvil.h"
#include "mc_atomic_file.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static uint64_t now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) {
        perror("clock");
        exit(1);
    }
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void sample(const char *phase, unsigned rep, uint64_t start, uint64_t bytes, unsigned operations) {
    printf("{\"phase\":\"%s\",\"rep\":%u,\"ns\":%" PRIu64 ",\"bytes\":%" PRIu64 ",\"operations\":%u}\n",
           phase, rep, now() - start, bytes, operations);
}
static void fail(const char *msg) {
    perror(msg);
    exit(1);
}
int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: %s REGION REPETITIONS [FRESH_DUMP_DIRECTORY]\n", argv[0]);
        return 2;
    }
    char *end;
    long reps = strtol(argv[2], &end, 10);
    if (*end || reps < 1 || reps > 10000)
        return 2;
    if (argc == 4 && mkdir(argv[3], 0700) != 0)
        fail("fresh dump directory");
    volatile uint64_t checksum = 0;
    for (unsigned rep = 0; rep < (unsigned)reps; rep++) {
        uint64_t start = now();
        int fd = open(argv[1], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0)
            fail("open");
        sample("open", rep, start, 0, 1);
        uint8_t header[8192];
        start = now();
        if (mc_file_pread_all(fd, header, sizeof(header), 0) != 0)
            fail("header");
        sample("header", rep, start, sizeof(header), 1);
        mc_region_index_t *index = NULL;
        start = now();
        if (mc_region_index_load(fd, &index) != 0)
            fail("index");
        sample("index", rep, start, 0, 1);
        const mc_region_stats_t *stats = mc_region_stats(index);
        if (!rep) {
            printf("{\"metadata\":true,\"file_bytes\":%" PRIu64
                   ",\"sectors\":%u,\"occupied\":%u,\"free\":%u,\"chunks\":%u,\"external\":%u,"
                   "\"unsupported\":%u,\"metadata_bytes\":%zu}\n",
                   stats->file_bytes, stats->sectors, stats->occupied, stats->free_sectors, stats->chunks,
                   stats->external, stats->unsupported, stats->metadata_bytes);
            for (unsigned s = 0; s < 1024; s++) {
                const mc_region_entry_t *e = mc_region_entry(index, s);
                if (e->offset)
                    printf("{\"entry\":%u,\"offset\":%u,\"count\":%u,\"length\":%u,\"codec\":%u,"
                           "\"timestamp\":%u}\n",
                           s, e->offset, e->sectors, e->length, e->compression, e->timestamp);
            }
            /* Independent oracle can check each sector, including unused holes. */
            printf("{\"used\":\"");
            for (uint32_t s = 0; s < stats->sectors; s++)
                putchar(mc_region_sector_used(index, s) ? '1' : '0');
            puts("\"}");
        }
        start = now();
        for (unsigned i = 0; i < 1000000; i++)
            checksum += mc_region_entry(index, i % 1024)->offset;
        sample("lookup", rep, start, 0, 1000000);
        unsigned first = 1024;
        for (unsigned s = 0; s < 1024; s++)
            if (mc_region_entry(index, s)->decodable) {
                first = s;
                break;
            }
        mc_region_index_free(index);
        if (close(fd) != 0)
            fail("close");
        mc_region_file_t *file = NULL;
        start = now();
        if (mc_region_open(argv[1], &file) != 0)
            fail("indexed open");
        sample("indexed_open", rep, start, 0, 1);
        if (first < 1024) {
            uint8_t *raw = NULL;
            size_t n = 0;
            start = now();
            if (mc_region_read_record(file, first, &raw, &n) != 0)
                fail("raw");
            sample("raw_chunk", rep, start, n, 1);
            free(raw);
            start = now();
            if (mc_anvil_read_chunk_indexed(file, first, &raw, &n) != 0)
                fail("decode");
            sample("decode_chunk", rep, start, n, 1);
            free(raw);
        }
        uint64_t bytes = 0;
        unsigned decoded = 0;
        start = now();
        for (unsigned s = 0; s < 1024; s++) {
            const mc_region_entry_t *e = mc_region_entry(mc_region_file_index(file), s);
            if (!e->offset)
                continue;
            uint8_t *raw = NULL;
            size_t n = 0;
            if (!e->decodable) {
                if (mc_region_read_record(file, s, &raw, &n) != 0)
                    fail("opaque record");
                free(raw);
                continue;
            }
            if (mc_anvil_read_chunk_indexed(file, s, &raw, &n) != 0)
                fail("decode all");
            bytes += n;
            decoded++;
            checksum += n;
            if (argc == 4 && !rep) {
                char path[4096];
                int len = snprintf(path, sizeof(path), "%s/%u.nbt", argv[3], s);
                if (len < 0 || (size_t)len >= sizeof(path)) {
                    errno = ENAMETOOLONG;
                    fail("dump");
                }
                int out = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
                if (out < 0)
                    fail("dump open");
                if (mc_file_write_all(out, raw, n) != 0 || close(out) != 0)
                    fail("dump write");
            }
            free(raw);
        }
        /* The first pass with a dump includes output I/O; exclude it from timing comparisons. */
        sample(argc == 4 && !rep ? "decode_all_with_dump" : "decode_all", rep, start, bytes, decoded);
        if (mc_region_close(file) != 0)
            fail("indexed close");
    }
    fprintf(stderr, "checksum=%" PRIu64 "\n", checksum);
    return 0;
}
