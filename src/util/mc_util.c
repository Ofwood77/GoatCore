#include "mc_util.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

bool mc_perf_enabled(void) {
    static int initialized = 0;
    static bool enabled = false;
    if (!initialized) {
        const char *env = getenv("MC_PERF");
        enabled = env && *env && strcmp(env, "0") != 0;
        initialized = 1;
    }
    return enabled;
}

int64_t mc_perf_slow_us(void) {
    static int initialized = 0;
    static int64_t slow_us = 10000;
    if (!initialized) {
        const char *env = getenv("MC_PERF_SLOW_MS");
        if (env && *env) {
            char *end = NULL;
            long ms = strtol(env, &end, 10);
            if (end != env && *end == '\0' && ms > 0) {
                slow_us = (int64_t)ms * 1000;
            }
        }
        initialized = 1;
    }
    return slow_us;
}
