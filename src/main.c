#include "mc_server.h"
#include "mc_util.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SERVER_CONFIG_PATH "server.config"
#define DEFAULT_LEVEL_SEED 0
#define DEFAULT_VIEW_DISTANCE 10
#define DEFAULT_SIMULATION_DISTANCE 8
#define DEFAULT_DIFFICULTY MC_DIFFICULTY_NORMAL

typedef struct {
    int64_t level_seed;
    int view_distance;
    int simulation_distance;
    mc_difficulty_t difficulty;
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
    fprintf(f, "difficulty=%s\n", mc_difficulty_name(DEFAULT_DIFFICULTY));
    if (fclose(f) != 0) {
        int e = errno;
        log_error("failed to write %s (errno=%d %s)", path, e, strerror(e));
        return -1;
    }
    return 0;
}

static int parse_difficulty_value(const char *val, mc_difficulty_t *out) {
    if (!val || !out) return -1;

    char tmp[32];
    size_t n = strlen(val);
    if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
    for (size_t i = 0; i < n; i++) tmp[i] = (char)tolower((unsigned char)val[i]);
    tmp[n] = '\0';

    if (strcmp(tmp, "0") == 0 || strcmp(tmp, "p") == 0 || strcmp(tmp, "peaceful") == 0) {
        *out = MC_DIFFICULTY_PEACEFUL;
        return 0;
    }
    if (strcmp(tmp, "1") == 0 || strcmp(tmp, "e") == 0 || strcmp(tmp, "easy") == 0) {
        *out = MC_DIFFICULTY_EASY;
        return 0;
    }
    if (strcmp(tmp, "2") == 0 || strcmp(tmp, "n") == 0 || strcmp(tmp, "normal") == 0) {
        *out = MC_DIFFICULTY_NORMAL;
        return 0;
    }
    if (strcmp(tmp, "3") == 0 || strcmp(tmp, "h") == 0 || strcmp(tmp, "hard") == 0) {
        *out = MC_DIFFICULTY_HARD;
        return 0;
    }
    return -1;
}

static int parse_server_config_file(FILE *f, server_disk_config_t *out) {
    if (!f || !out) return -1;
    *out = (server_disk_config_t){0};
    out->level_seed = DEFAULT_LEVEL_SEED;
    out->view_distance = DEFAULT_VIEW_DISTANCE;
    out->simulation_distance = DEFAULT_SIMULATION_DISTANCE;
    out->difficulty = DEFAULT_DIFFICULTY;

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
        if (strcmp(key, "difficulty") == 0) {
            if (parse_difficulty_value(val, &out->difficulty) != 0) {
                log_error("invalid difficulty in %s: '%s' (expected peaceful/easy/normal/hard)", SERVER_CONFIG_PATH, val);
                return -1;
            }
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

static int require_runtime_asset(const char *path, const char *label) {
    if (!path || !*path) {
        log_error("missing %s path in server configuration", label ? label : "runtime asset");
        return -1;
    }
    if (access(path, R_OK) != 0) {
        int e = errno;
        log_error("required %s missing or unreadable: %s (errno=%d %s)", label ? label : "runtime asset", path, e, strerror(e));
        return -1;
    }
    return 0;
}

static int parse_env_u16(const char *name, uint16_t *out) {
    const char *env = getenv(name);
    if (!env || !*env) return 0;
    char *end = NULL;
    errno = 0;
    long value = strtol(env, &end, 10);
    if (errno != 0 || end == env || *end != '\0' || value <= 0 || value > 65535) {
        log_error("invalid %s override: '%s' (expected 1..65535)", name, env);
        return -1;
    }
    *out = (uint16_t)value;
    return 1;
}

int main(void) {
    const char *ignored = getenv("MC_WORLD_SEED");
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
    cfg.registry_blob_path = "assets/registry_packets_26_1_1.bin";
    cfg.tags_blob_path = "assets/tags_packet_26_1_1.bin";
    cfg.chunk_blob_path = "assets/chunk_0_0_26_1_1.bin";
    cfg.block_states_path = "assets/block_states.json";
    cfg.world_path = "world";
    cfg.level_seed = disk_cfg.level_seed;
    cfg.view_distance = disk_cfg.view_distance;
    cfg.simulation_distance = disk_cfg.simulation_distance;
    cfg.difficulty = disk_cfg.difficulty;

    const char *env_world_path = getenv("MC_WORLD_PATH");
    if (env_world_path && *env_world_path) {
        cfg.world_path = env_world_path;
        log_info("MC_WORLD_PATH override: %s", cfg.world_path);
    }

    uint16_t env_bind_port = 0;
    int port_override = parse_env_u16("MC_BIND_PORT", &env_bind_port);
    if (port_override < 0) return 1;
    if (port_override > 0) {
        cfg.bind_port = env_bind_port;
        log_info("MC_BIND_PORT override: %u", cfg.bind_port);
    }

    if (require_runtime_asset(cfg.registry_blob_path, "registry blob") != 0 ||
        require_runtime_asset(cfg.tags_blob_path, "tags blob") != 0 ||
        require_runtime_asset(cfg.chunk_blob_path, "spawn chunk blob") != 0) {
        return 1;
    }

    mc_server_t *srv = NULL;
    if (net_server_init(&srv, &cfg) != 0) {
        log_error("failed to init server");
        return 1;
    }

    /* Do not modify this boot disclaimer block. */
    log_info("This server is meant to simulate a vanilla server, but differences may still be noticeable");
    log_info("I do not know why this works or not");
    log_info("OFWOOD");
    log_info("server listening on port %u", cfg.bind_port);
    int rc = net_server_run(srv);
    net_server_destroy(srv);

    return rc == 0 ? 0 : 2;
}
