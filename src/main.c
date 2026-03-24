#include "mc_server.h"
#include "mc_util.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SERVER_CONFIG_PATH "server.config"
#define DEFAULT_LEVEL_SEED 0
#define DEFAULT_VIEW_DISTANCE 10
#define DEFAULT_SIMULATION_DISTANCE 8

typedef struct {
    int64_t level_seed;
    int view_distance;
    int simulation_distance;
} server_disk_config_t;

static void trim_ws(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[n - 1] = '\0';
        n--;
    }
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i > 0) memmove(s, s + i, strlen(s + i) + 1);
}

static int write_default_server_config(const char *path) {
    if (!path) return -1;
    FILE *f = fopen(path, "w");
    if (!f) {
        int e = errno;
        log_error("failed to create %s (errno=%d %s)", path, e, strerror(e));
        return -1;
    }
    fprintf(f, "# mc_c_server config\n");
    fprintf(f, "level-seed=%d\n", DEFAULT_LEVEL_SEED);
    fprintf(f, "view-distance=%d\n", DEFAULT_VIEW_DISTANCE);
    fprintf(f, "simulation-distance=%d\n", DEFAULT_SIMULATION_DISTANCE);
    if (fclose(f) != 0) {
        int e = errno;
        log_error("failed to write %s (errno=%d %s)", path, e, strerror(e));
        return -1;
    }
    return 0;
}

static int parse_server_config_file(FILE *f, server_disk_config_t *out) {
    if (!f || !out) return -1;
    *out = (server_disk_config_t){0};
    out->level_seed = DEFAULT_LEVEL_SEED;
    out->view_distance = DEFAULT_VIEW_DISTANCE;
    out->simulation_distance = DEFAULT_SIMULATION_DISTANCE;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        trim_ws(line);
        if (line[0] == '\0') continue;
        if (line[0] == '#') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        char *hash = strchr(val, '#');
        if (hash) *hash = '\0';
        trim_ws(key);
        trim_ws(val);
        if (!*key) continue;

        if (strcmp(key, "level-seed") == 0) {
            long long v = 0;
            if (!*val || sscanf(val, "%lld", &v) != 1) {
                log_error("invalid level-seed in %s: '%s'", SERVER_CONFIG_PATH, val);
                return -1;
            }
            out->level_seed = (int64_t)v;
            continue;
        }
        if (strcmp(key, "view-distance") == 0) {
            int v = 0;
            if (!*val || sscanf(val, "%d", &v) != 1 || v < 2 || v > 32) {
                log_error("invalid view-distance in %s: '%s' (expected 2..32)", SERVER_CONFIG_PATH, val);
                return -1;
            }
            out->view_distance = v;
            continue;
        }
        if (strcmp(key, "simulation-distance") == 0) {
            int v = 0;
            if (!*val || sscanf(val, "%d", &v) != 1 || v < 2 || v > 32) {
                log_error("invalid simulation-distance in %s: '%s' (expected 2..32)", SERVER_CONFIG_PATH, val);
                return -1;
            }
            out->simulation_distance = v;
            continue;
        }

        log_info("server.config: ignoring unknown key '%s'", key);
    }

    return 0;
}

static int load_server_config(const char *path, server_disk_config_t *out) {
    if (!path || !out) return -1;
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno != ENOENT) {
            int e = errno;
            log_error("failed to open %s (errno=%d %s)", path, e, strerror(e));
            return -1;
        }
        log_info("%s missing; creating defaults", path);
        if (write_default_server_config(path) != 0) return -1;
        f = fopen(path, "r");
        if (!f) {
            int e = errno;
            log_error("failed to open %s after creating (errno=%d %s)", path, e, strerror(e));
            return -1;
        }
    }

    int rc = parse_server_config_file(f, out);
    fclose(f);
    return rc;
}

int main(void) {
    const char *ignored = getenv("MC_WORLD_PATH");
    if (ignored && *ignored) {
        log_info("MC_WORLD_PATH ignored; world is always ./world");
    }
    ignored = getenv("MC_WORLD_SEED");
    if (ignored && *ignored) {
        log_info("MC_WORLD_SEED ignored; use %s level-seed=", SERVER_CONFIG_PATH);
    }

    server_disk_config_t disk_cfg;
    if (load_server_config(SERVER_CONFIG_PATH, &disk_cfg) != 0) {
        log_error("failed to load %s", SERVER_CONFIG_PATH);
        return 1;
    }

    mc_server_config_t cfg;
    cfg.bind_ip = NULL;
    cfg.bind_port = 25565;
    cfg.max_connections = 128;
    cfg.compression_threshold = -1;
    cfg.online_mode = false;
    cfg.debug_packets = true;
    cfg.registry_blob_path = "assets/registry_packets_1_21_1.bin";
    cfg.chunk_blob_path = "assets/chunk_0_0.bin";
    cfg.block_states_path = "assets/block_states.json";
    cfg.world_path = "world";
    cfg.level_seed = disk_cfg.level_seed;
    cfg.view_distance = disk_cfg.view_distance;
    cfg.simulation_distance = disk_cfg.simulation_distance;

    mc_server_t *srv = NULL;
    if (net_server_init(&srv, &cfg) != 0) {
        log_error("failed to init server");
        return 1;
    }

    log_info("server listening on port %u", cfg.bind_port);
    int rc = net_server_run(srv);
    net_server_destroy(srv);

    return rc == 0 ? 0 : 2;
}
