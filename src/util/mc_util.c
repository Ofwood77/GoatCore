#include "mc_util.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <stdint.h>
#include <errno.h>

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

static pthread_once_t save_trace_once = PTHREAD_ONCE_INIT;
static FILE *save_trace;
static void init_save_trace(void) {
    const char *path = getenv("MC_SAVE_TRACE_PATH");
    if (!path || !*path) return;
    save_trace = fopen(path, "w");
    if (!save_trace) { log_error("cannot open save trace"); return; }
    setvbuf(save_trace, NULL, _IOFBF, 65536);
    fprintf(save_trace, "start_us,end_us,cpu_us,thread,phase,cx,cz,bytes,result\n");
}
static int64_t save_thread_cpu_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
mc_save_clock_t mc_save_profile_begin(void) {
    int saved_errno = errno;
    pthread_once(&save_trace_once, init_save_trace);
    mc_save_clock_t result = {0};
    if (save_trace) result = (mc_save_clock_t){mc_now_us(), save_thread_cpu_us()};
    errno = saved_errno;
    return result;
}
void mc_save_profile_end(const char *phase, int32_t cx, int32_t cz,
                         mc_save_clock_t start, size_t bytes, int result) {
    if (!start.wall_us) return;
    int saved_errno = errno;
    int64_t end = mc_now_us(), cpu = save_thread_cpu_us() - start.cpu_us;
    /* fprintf serializes only this diagnostic stream, not world ownership. */
    fprintf(save_trace, "%lld,%lld,%lld,%lu,%s,%d,%d,%zu,%d\n",
            (long long)start.wall_us, (long long)end, (long long)cpu,
            (unsigned long)pthread_self(), phase, cx, cz, bytes, result);
    errno = saved_errno;
}
