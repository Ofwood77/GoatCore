#include "mc_util.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <stdint.h>

void log_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stdout, "[info] ");
    vfprintf(stdout, fmt, ap);
    fprintf(stdout, "\n");
    va_end(ap);
}

void log_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[error] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

void hex_dump(const uint8_t *data, size_t len) {
    if (!data) return;
    size_t max = len > 64 ? 64 : len;
    fprintf(stdout, "[hex] ");
    for (size_t i = 0; i < max; i++) {
        fprintf(stdout, "%02X ", data[i]);
    }
    if (len > max) fprintf(stdout, "... (%zu bytes)", len);
    fprintf(stdout, "\n");
}

uint16_t read_be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

void write_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xff);
    p[1] = (uint8_t)(v & 0xff);
}

int read_file(const char *path, uint8_t **out, size_t *out_len) {
    if (!path || !out || !out_len) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long sz = ftell(f);
    if (sz <= 0) {
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return -1;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) {
        free(buf);
        return -1;
    }
    *out = buf;
    *out_len = n;
    return 0;
}

int64_t mc_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + (ts.tv_nsec / 1000);
}

static pthread_once_t perf_config_once = PTHREAD_ONCE_INIT;
static bool perf_enabled = false;
static int64_t perf_slow_us = 10000;
static uint64_t perf_summary_ticks = 200;

static void init_perf_config(void) {
    const char *env = getenv("MC_PERF");
    perf_enabled = env && *env && strcmp(env, "0") != 0;

    env = getenv("MC_PERF_SLOW_MS");
    if (env && *env) {
        char *end = NULL;
        long ms = strtol(env, &end, 10);
        if (end != env && *end == '\0' && ms > 0 && (uint64_t)ms <= INT64_MAX / 1000u) {
            perf_slow_us = (int64_t)ms * 1000;
        }
    }
    env = getenv("MC_PERF_SUMMARY_TICKS");
    if (env && *env) {
        char *end = NULL;
        unsigned long long value = strtoull(env, &end, 10);
        if (end != env && *end == '\0') perf_summary_ticks = (uint64_t)value;
    }
}

bool mc_perf_enabled(void) {
    pthread_once(&perf_config_once, init_perf_config);
    return perf_enabled;
}

int64_t mc_perf_slow_us(void) {
    pthread_once(&perf_config_once, init_perf_config);
    return perf_slow_us;
}

uint64_t mc_perf_summary_ticks(void) {
    pthread_once(&perf_config_once, init_perf_config);
    return perf_summary_ticks;
}
