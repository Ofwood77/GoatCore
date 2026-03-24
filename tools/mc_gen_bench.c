#include "mc_world.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int parse_i64(const char *s, int64_t *out) {
    if (!s || !*s || !out) return -1;
    errno = 0;
    char *end = NULL;
    long long v = strtoll(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return -1;
    *out = (int64_t)v;
    return 0;
}

static int parse_i32(const char *s, int32_t *out) {
    int64_t v = 0;
    if (parse_i64(s, &v) != 0) return -1;
    if (v < INT32_MIN || v > INT32_MAX) return -1;
    *out = (int32_t)v;
    return 0;
}

static int parse_f32(const char *s, float *out) {
    if (!s || !*s || !out) return -1;
    errno = 0;
    char *end = NULL;
    double v = strtod(s, &end);
    if (errno != 0 || end == s || *end != '\0') return -1;
    *out = (float)v;
    return 0;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--chunks N] [--seed S] [--freq F] [--amp A]\n"
            "  defaults: N=1024 S=0 F=0.01 A=10\n",
            argv0 ? argv0 : "mc_gen_bench");
}

int main(int argc, char **argv) {
    int32_t chunks = 1024;
    int64_t seed = 0;
    float freq = 0.01f;
    int32_t amp = 10;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--chunks") == 0 && i + 1 < argc) {
            if (parse_i32(argv[++i], &chunks) != 0 || chunks <= 0) {
                usage(argv[0]);
                return 2;
            }
            continue;
        }
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            if (parse_i64(argv[++i], &seed) != 0) {
                usage(argv[0]);
                return 2;
            }
            continue;
        }
        if (strcmp(argv[i], "--freq") == 0 && i + 1 < argc) {
            if (parse_f32(argv[++i], &freq) != 0 || !(freq > 0.0f)) {
                usage(argv[0]);
                return 2;
            }
            continue;
        }
        if (strcmp(argv[i], "--amp") == 0 && i + 1 < argc) {
            if (parse_i32(argv[++i], &amp) != 0 || amp < 0) {
                usage(argv[0]);
                return 2;
            }
            continue;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        }
        usage(argv[0]);
        return 2;
    }

    char seed_buf[64];
    snprintf(seed_buf, sizeof(seed_buf), "%" PRId64, seed);
    (void)seed_buf;

    mc_world_t *w = mc_world_create(NULL, seed);
    if (!w) {
        fprintf(stderr, "FAIL: mc_world_create\n");
        return 1;
    }
    mc_world_set_generation_params(w, freq, 64, amp);
    const mc_world_ids_t *ids = mc_world_ids(w);
    int32_t air = ids ? ids->air : 0;

    uint64_t min_ns = UINT64_MAX;
    uint64_t max_ns = 0;
    __uint128_t sum_ns = 0;

    for (int32_t i = 0; i < chunks; i++) {
        int32_t cx = (i % 32) - 16;
        int32_t cz = (i / 32) - 16;
        mc_chunk_t c;
        memset(&c, 0, sizeof(c));
        c.cx = cx;
        c.cz = cz;
        for (size_t bi = 0; bi < MC_BLOCKS_PER_CHUNK; bi++) {
            c.blocks[bi] = air;
        }

        uint64_t t0 = now_ns();
        mc_world_generate_chunk(w, &c);
        uint64_t t1 = now_ns();
        uint64_t dt = t1 - t0;

        if (dt < min_ns) min_ns = dt;
        if (dt > max_ns) max_ns = dt;
        sum_ns += dt;
    }

    double avg_ns = (double)sum_ns / (double)chunks;
    printf("chunks=%d seed=%" PRId64 " freq=%.6f amp=%d\n", chunks, seed, freq, amp);
    printf("min=%.2f us avg=%.2f us max=%.2f us\n", (double)min_ns / 1000.0, avg_ns / 1000.0,
           (double)max_ns / 1000.0);

    mc_world_destroy(w);
    return 0;
}
