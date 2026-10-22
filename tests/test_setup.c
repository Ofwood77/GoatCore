#define _GNU_SOURCE
#include "mc_setup.h"

#include <assert.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_setup(const char *path, const char *body) {
    FILE *file = fopen(path, "w");
    assert(file);
    assert(fputs(body, file) >= 0);
    assert(fclose(file) == 0);
}

int main(void) {
    char path[] = "/tmp/goatcore_setup_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    char error[512] = {0};
    mc_setup_config_t cfg;

    write_setup(path, "[performance]\ncpu_cores = 0\ncpu_affinity = auto\nchunk_workers = auto\ntick_rate = 20\n");
    assert(mc_setup_load(path, true, &cfg, error, sizeof(error)) == 0);
    assert(cfg.cpu_cores == 0 && cfg.chunk_workers == 0 && cfg.tick_rate == 20);

    cpu_set_t original;
    assert(sched_getaffinity(0, sizeof(original), &original) == 0);
    int first = -1;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (CPU_ISSET(cpu, &original)) { first = cpu; break; }
    }
    assert(first >= 0);
    char body[256];
    snprintf(body, sizeof(body), "[performance]\ncpu_cores = 1\ncpu_affinity = %d\nchunk_workers = 2\ntick_rate = 20\n", first);
    write_setup(path, body);
    assert(mc_setup_load(path, true, &cfg, error, sizeof(error)) == 0);
    assert(mc_setup_apply_affinity(&cfg, error, sizeof(error)) == 0);
    assert(cfg.linux_available_cpus == CPU_COUNT(&original));
    assert(cfg.allowed_cpus == 1);
    assert(mc_setup_resolve_chunk_workers(&cfg) == 2);
    cpu_set_t actual;
    assert(sched_getaffinity(0, sizeof(actual), &actual) == 0);
    assert(CPU_COUNT(&actual) == 1 && CPU_ISSET(first, &actual));
    assert(sched_setaffinity(0, sizeof(original), &original) == 0);

    int unavailable = -1;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (!CPU_ISSET(cpu, &original)) { unavailable = cpu; break; }
    }
    if (unavailable >= 0) {
        snprintf(body, sizeof(body), "[performance]\ncpu_affinity = %d\n", unavailable);
        write_setup(path, body);
        assert(mc_setup_load(path, true, &cfg, error, sizeof(error)) == 0);
        assert(mc_setup_apply_affinity(&cfg, error, sizeof(error)) != 0);
        assert(strstr(error, "not allowed"));
    }

    write_setup(path, "[performance]\ncpu_cores = 99999\n");
    assert(mc_setup_load(path, true, &cfg, error, sizeof(error)) != 0);
    for (int n = 1; n <= 4; n *= 2) {
        snprintf(body, sizeof(body), "[performance]\nnetwork_workers = %d\n", n);
        write_setup(path, body);
        assert(mc_setup_load(path, true, &cfg, error, sizeof(error)) == 0);
        assert(cfg.network_workers == n);
    }
    write_setup(path, "[performance]\nnetwork_workers = 3\n");
    assert(mc_setup_load(path, true, &cfg, error, sizeof(error)) != 0);
    write_setup(path, "[performance]\nsimulation_workers = 1\n");
    assert(mc_setup_load(path, true, &cfg, error, sizeof(error)) != 0);
    assert(strstr(error, "not implemented"));
    write_setup(path, "[performance]\ntick_rate = 40\n");
    assert(mc_setup_load(path, true, &cfg, error, sizeof(error)) != 0);
    write_setup(path, "[performance]\nchunk_workers = 17\n");
    assert(mc_setup_load(path, true, &cfg, error, sizeof(error)) != 0);
    write_setup(path, "[performance]\ncpu_affinity = 0-\n");
    assert(mc_setup_load(path, true, &cfg, error, sizeof(error)) == 0);
    assert(mc_setup_apply_affinity(&cfg, error, sizeof(error)) != 0);
    assert(strstr(error, "invalid cpu_affinity"));

    assert(unlink(path) == 0);
    assert(mc_setup_load(path, true, &cfg, error, sizeof(error)) != 0);
    assert(mc_setup_load(path, false, &cfg, error, sizeof(error)) == 0);
    puts("test_setup: ok");
    return 0;
}
