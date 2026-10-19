#define _GNU_SOURCE
#include "mc_setup.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fail(char *error, size_t cap, const char *fmt, ...) {
    if (error && cap) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(error, cap, fmt, args);
        va_end(args);
    }
    return -1;
}

static char *trim(char *value) {
    while (isspace((unsigned char)*value)) value++;
    size_t len = strlen(value);
    while (len && isspace((unsigned char)value[len - 1])) value[--len] = '\0';
    return value;
}

static int parse_uint(const char *value, int min, int max, int *out) {
    if (!value || !*value || !out) return -1;
    for (const char *p = value; *p; p++) if (!isdigit((unsigned char)*p)) return -1;
    errno = 0;
    char *end = NULL;
    long number = strtol(value, &end, 10);
    if (errno || !end || *end || number < min || number > max) return -1;
    *out = (int)number;
    return 0;
}

void mc_setup_defaults(mc_setup_config_t *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->cpu_affinity, sizeof(cfg->cpu_affinity), "auto");
    cfg->tick_rate = 20;
}

int mc_setup_load(const char *path, bool required, mc_setup_config_t *cfg, char *error, size_t error_cap) {
    if (!path || !*path || !cfg) return fail(error, error_cap, "setup path or output is missing");
    mc_setup_defaults(cfg);
    FILE *file = fopen(path, "r");
    if (!file) {
        if (!required && errno == ENOENT) return 0;
        return fail(error, error_cap, "cannot open %s: %s", path, strerror(errno));
    }
    bool in_performance = false;
    unsigned seen = 0;
    int line_no = 0;
    int result = 0;
    char line[2048];
    while (fgets(line, sizeof(line), file)) {
        line_no++;
        if (!strchr(line, '\n') && !feof(file)) {
            result = fail(error, error_cap, "%s:%d: line too long", path, line_no);
            break;
        }
        char *comment = strpbrk(line, "#;");
        if (comment) *comment = '\0';
        char *key = trim(line);
        if (!*key) continue;
        if (*key == '[') {
            if (strcmp(key, "[performance]") != 0) {
                result = fail(error, error_cap, "%s:%d: unsupported section %s", path, line_no, key);
                break;
            }
            if (in_performance) {
                result = fail(error, error_cap, "%s:%d: duplicate [performance] section", path, line_no);
                break;
            }
            in_performance = true;
            continue;
        }
        if (!in_performance) {
            result = fail(error, error_cap, "%s:%d: expected [performance] section", path, line_no);
            break;
        }
        char *equals = strchr(key, '=');
        if (!equals) {
            result = fail(error, error_cap, "%s:%d: expected key = value", path, line_no);
            break;
        }
        *equals = '\0';
        char *value = trim(equals + 1);
        key = trim(key);
        int bit = 0;
        if (strcmp(key, "cpu_cores") == 0) bit = 1;
        else if (strcmp(key, "cpu_affinity") == 0) bit = 2;
        else if (strcmp(key, "chunk_workers") == 0) bit = 4;
        else if (strcmp(key, "tick_rate") == 0) bit = 8;
        else if (strcmp(key, "network_workers") == 0 || strcmp(key, "save_workers") == 0 ||
                 strcmp(key, "simulation_workers") == 0) {
            result = fail(error, error_cap, "%s:%d: %s is not implemented", path, line_no, key);
            break;
        } else {
            result = fail(error, error_cap, "%s:%d: unknown key %s", path, line_no, key);
            break;
        }
        if (seen & (unsigned)bit) {
            result = fail(error, error_cap, "%s:%d: duplicate key %s", path, line_no, key);
            break;
        }
        seen |= (unsigned)bit;
        if (bit == 1 && parse_uint(value, 0, CPU_SETSIZE, &cfg->cpu_cores) != 0)
            result = fail(error, error_cap, "%s:%d: cpu_cores must be 0..%d", path, line_no, CPU_SETSIZE);
        else if (bit == 2) {
            if (!*value || strlen(value) >= sizeof(cfg->cpu_affinity))
                result = fail(error, error_cap, "%s:%d: invalid cpu_affinity", path, line_no);
            else snprintf(cfg->cpu_affinity, sizeof(cfg->cpu_affinity), "%s", value);
        } else if (bit == 4 && strcmp(value, "auto") != 0 &&
                   parse_uint(value, 1, MC_SETUP_MAX_CHUNK_WORKERS, &cfg->chunk_workers) != 0)
            result = fail(error, error_cap, "%s:%d: chunk_workers must be auto or 1..%d",
                          path, line_no, MC_SETUP_MAX_CHUNK_WORKERS);
        else if (bit == 8 && parse_uint(value, 20, 20, &cfg->tick_rate) != 0)
            result = fail(error, error_cap, "%s:%d: only tick_rate = 20 is supported", path, line_no);
        if (result) break;
    }
    if (!result && ferror(file)) result = fail(error, error_cap, "cannot read %s: %s", path, strerror(errno));
    if (!result && !in_performance) result = fail(error, error_cap, "%s: missing [performance] section", path);
    fclose(file);
    return result;
}

static int parse_cpu_id(const char **cursor, int *id) {
    const char *start = *cursor;
    if (!isdigit((unsigned char)*start)) return -1;
    unsigned long value = 0;
    while (isdigit((unsigned char)**cursor)) {
        if (value > (CPU_SETSIZE - 1u) / 10u) return -1;
        value = value * 10 + (unsigned)(**cursor - '0');
        if (value >= CPU_SETSIZE) return -1;
        (*cursor)++;
    }
    *id = (int)value;
    return 0;
}

static int parse_cpu_mask(const char *value, cpu_set_t *out) {
    CPU_ZERO(out);
    const char *cursor = value;
    if (!*cursor) return -1;
    for (;;) {
        while (*cursor == ' ' || *cursor == '\t') cursor++;
        int first = 0;
        if (parse_cpu_id(&cursor, &first) != 0) return -1;
        while (*cursor == ' ' || *cursor == '\t') cursor++;
        int last = first;
        if (*cursor == '-') {
            cursor++;
            while (*cursor == ' ' || *cursor == '\t') cursor++;
            if (parse_cpu_id(&cursor, &last) != 0 || last < first) return -1;
        }
        for (int cpu = first; cpu <= last; cpu++) CPU_SET(cpu, out);
        while (*cursor == ' ' || *cursor == '\t') cursor++;
        if (!*cursor) return 0;
        if (*cursor != ',') return -1;
        cursor++;
        if (!*cursor) return -1;
    }
}

int mc_setup_apply_affinity(mc_setup_config_t *cfg, char *error, size_t error_cap) {
    if (!cfg) return fail(error, error_cap, "setup configuration is missing");
    cpu_set_t linux_mask;
    if (sched_getaffinity(0, sizeof(linux_mask), &linux_mask) != 0)
        return fail(error, error_cap, "sched_getaffinity failed: %s", strerror(errno));
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    cfg->linux_online_cpus = online > 0 && online <= INT_MAX ? (int)online : 0;
    cfg->linux_available_cpus = CPU_COUNT(&linux_mask);
    cpu_set_t selected;
    if (strcmp(cfg->cpu_affinity, "auto") == 0) selected = linux_mask;
    else if (parse_cpu_mask(cfg->cpu_affinity, &selected) != 0)
        return fail(error, error_cap, "invalid cpu_affinity: %s", cfg->cpu_affinity);
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (CPU_ISSET(cpu, &selected) && !CPU_ISSET(cpu, &linux_mask))
            return fail(error, error_cap, "CPU %d is not allowed by Linux/cpuset", cpu);
    }
    int count = CPU_COUNT(&selected);
    if (!count) return fail(error, error_cap, "cpu_affinity selects no CPU");
    if (cfg->cpu_cores > count)
        return fail(error, error_cap, "cpu_cores=%d exceeds %d selected logical CPUs", cfg->cpu_cores, count);
    if (cfg->cpu_cores > 0) {
        int kept = 0;
        for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
            if (!CPU_ISSET(cpu, &selected)) continue;
            if (kept++ >= cfg->cpu_cores) CPU_CLR(cpu, &selected);
        }
    }
    if (sched_setaffinity(0, sizeof(selected), &selected) != 0)
        return fail(error, error_cap, "sched_setaffinity failed: %s", strerror(errno));
    cpu_set_t actual;
    if (sched_getaffinity(0, sizeof(actual), &actual) != 0 || !CPU_EQUAL(&selected, &actual))
        return fail(error, error_cap, "effective CPU affinity differs from the requested mask");
    cfg->allowed_cpus = CPU_COUNT(&actual);
    size_t used = 0;
    cfg->allowed_cpu_list[0] = '\0';
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (!CPU_ISSET(cpu, &actual)) continue;
        int n = snprintf(cfg->allowed_cpu_list + used, sizeof(cfg->allowed_cpu_list) - used,
                         "%s%d", used ? "," : "", cpu);
        if (n < 0 || (size_t)n >= sizeof(cfg->allowed_cpu_list) - used)
            return fail(error, error_cap, "effective CPU list is too long");
        used += (size_t)n;
    }
    return 0;
}

int mc_setup_resolve_chunk_workers(const mc_setup_config_t *cfg) {
    if (!cfg) return 1;
    if (cfg->chunk_workers > 0) return cfg->chunk_workers;
    int workers = cfg->allowed_cpus - 2; /* leave scheduling room for network + tick */
    if (workers < 1) workers = 1;
    if (workers > 4) workers = 4;
    return workers;
}
