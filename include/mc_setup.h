#ifndef MC_SETUP_H
#define MC_SETUP_H

#include <stdbool.h>
#include <stddef.h>

#define MC_SETUP_AFFINITY_LEN 1024
#define MC_SETUP_CPU_LIST_LEN 4096
#define MC_SETUP_MAX_CHUNK_WORKERS 16

typedef struct {
    int cpu_cores; /* 0: every CPU in the selected Linux mask */
    char cpu_affinity[MC_SETUP_AFFINITY_LEN]; /* auto or logical CPU list/ranges */
    int network_workers; /* 1, 2 or 4 independent epoll owners */
    int chunk_workers; /* 0: automatic; otherwise 1..MC_SETUP_MAX_CHUNK_WORKERS */
    int tick_rate; /* only 20 is currently supported */
    int linux_online_cpus;
    int linux_available_cpus;
    int allowed_cpus;
    char allowed_cpu_list[MC_SETUP_CPU_LIST_LEN];
} mc_setup_config_t;

void mc_setup_defaults(mc_setup_config_t *cfg);
int mc_setup_load(const char *path, bool required, mc_setup_config_t *cfg, char *error, size_t error_cap);
/* Call before creating any threads. Linux threads created afterwards inherit this affinity. */
int mc_setup_apply_affinity(mc_setup_config_t *cfg, char *error, size_t error_cap);
int mc_setup_resolve_chunk_workers(const mc_setup_config_t *cfg);

#endif
