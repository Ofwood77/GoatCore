#include "mc_memory.h"
#include "mc_util.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#define MIB UINT64_C(1048576)

static int number(const char *text, uint64_t *result) {
    if (!text || !*text) return -1;
    for (const char *p = text; *p; p++) if (*p < '0' || *p > '9') return -1;
    char *end;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || *end) return -1;
    *result = value;
    return 0;
}

int mc_memory_parse_mib(const char *text, uint64_t *mib) {
    uint64_t value;
    if (!mib || number(text, &value) || value < MC_MEMORY_DEFAULT_MIB || value > (UINT64_MAX - 1) / MIB) {
        errno = EINVAL; return -1;
    }
    *mib = value;
    return 0;
}

int mc_memory_plan(uint64_t requested_mib, uint64_t available, uint64_t headroom, uint64_t *bytes) {
    if (!bytes || requested_mib < MC_MEMORY_DEFAULT_MIB || requested_mib > (UINT64_MAX - 1) / MIB) {
        errno = EINVAL; return -1;
    }
    uint64_t usable = available < headroom ? available : headroom;
    if (usable <= MC_MEMORY_RESERVE_BYTES) { errno = ENOMEM; return -1; }
    usable -= MC_MEMORY_RESERVE_BYTES;
    uint64_t requested = requested_mib * MIB;
    *bytes = requested < usable ? requested : usable;
    return 0;
}

static int read_limit(const char *path, uint64_t *out) {
    FILE *file = fopen(path, "r");
    if (!file) return -1;
    char token[64];
    int result = fscanf(file, "%63s", token);
    int closed = fclose(file);
    if (result != 1 || closed) return -1;
    if (!strcmp(token, "max")) { *out = UINT64_MAX; return 0; }
    return number(token, out);
}

static int resources(uint64_t *available, uint64_t *headroom) {
    FILE *file = fopen("/proc/meminfo", "r");
    if (!file) return -1;
    char line[4096];
    *available = 0;
    while (fgets(line, sizeof(line), file)) {
        uint64_t kib;
        if (sscanf(line, "MemAvailable: %" SCNu64 " kB", &kib) == 1 && kib <= UINT64_MAX / 1024)
            *available = kib * 1024;
    }
    if (fclose(file) || !*available) return -1;
    file = fopen("/proc/self/cgroup", "r");
    if (!file) return -1;
    char group[4096] = {0};
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, "0::/", 4)) continue;
        line[strcspn(line, "\r\n")] = 0;
        if (snprintf(group, sizeof(group), "/sys/fs/cgroup%s", line + 3) >= (int)sizeof(group)) { fclose(file); return -1; }
        break;
    }
    fclose(file);
    /* Unknown/v1 hierarchy is refused rather than silently assuming no cap. */
    if (!group[0]) { errno = ENOTSUP; return -1; }
    size_t root = strlen("/sys/fs/cgroup");
    if (group[strlen(group)-1] == '/') group[strlen(group)-1] = 0;
    *headroom = UINT64_MAX;
    for (;;) {
        char path[4160]; uint64_t limit, current;
        snprintf(path, sizeof(path), "%s/memory.max", group);
        if (read_limit(path, &limit)) return -1;
        snprintf(path, sizeof(path), "%s/memory.current", group);
        if (read_limit(path, &current)) return -1;
        uint64_t left = limit == UINT64_MAX ? UINT64_MAX : current < limit ? limit - current : 0;
        if (left < *headroom) *headroom = left;
        if (strlen(group) == root) break;
        char *slash = strrchr(group, '/');
        if (!slash || (size_t)(slash-group) < root) return -1;
        *slash = 0;
    }
    return 0;
}

int mc_memory_apply(uint64_t requested_mib) {
    uint64_t available, headroom, bytes;
    struct rlimit limit;
    if (resources(&available, &headroom) || mc_memory_plan(requested_mib, available, headroom, &bytes) || getrlimit(RLIMIT_AS, &limit)) {
        log_error("memory budget: cannot determine safe memory/cgroup-v2 limits errno=%d", errno);
        return -1;
    }
    if (limit.rlim_max != RLIM_INFINITY && bytes > limit.rlim_max) bytes = limit.rlim_max;
    if (limit.rlim_cur != RLIM_INFINITY && bytes > limit.rlim_cur) bytes = limit.rlim_cur;
    if ((uint64_t)(rlim_t)bytes != bytes) return -1;
    limit.rlim_cur = (rlim_t)bytes;
    if (setrlimit(RLIMIT_AS, &limit)) { log_error("memory budget: setrlimit failed errno=%d", errno); return -1; }
    log_info("memory budget requested_mib=%" PRIu64 " effective_as_mib=%" PRIu64 " available_mib=%" PRIu64 " cgroup_headroom_mib=%" PRIu64 " reserve_mib=512; no preallocation",
        requested_mib, bytes/MIB, available/MIB, headroom == UINT64_MAX ? UINT64_MAX : headroom/MIB);
    if (bytes < requested_mib*MIB) log_info("memory budget reduced to respect available RAM, cgroup or inherited address-space limit");
    return 0;
}
