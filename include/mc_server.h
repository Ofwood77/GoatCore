#ifndef MC_SERVER_H
#define MC_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "mc_inventory.h"

#define MC_PROTO_VERSION 775
#define MC_PROTO_VERSION_1_21_1 MC_PROTO_VERSION
#define MC_GAME_VERSION "26.1"

typedef enum {
    MC_STATE_HANDSHAKING = 0,
    MC_STATE_STATUS = 1,
    MC_STATE_LOGIN = 2,
    MC_STATE_PLAY = 3,
    MC_STATE_CONFIGURATION = 4
} mc_proto_state_t;

typedef struct {
    const char *bind_ip;
    uint16_t bind_port;
    int max_connections;
    int compression_threshold; /* -1 disables compression */
    bool online_mode;
    bool debug_packets;
    const char *registry_blob_path;
    const char *tags_blob_path;
    const char *chunk_blob_path;
    const char *block_states_path;
    const char *world_path; /* optional: path to world dir containing region/ */
    int64_t level_seed;
    int view_distance;
    int simulation_distance;
} mc_server_config_t;

typedef struct mc_server mc_server_t;

int net_server_init(mc_server_t **out, const mc_server_config_t *cfg);
int net_server_run(mc_server_t *server);
void net_server_stop(mc_server_t *server);
void net_server_destroy(mc_server_t *server);
int net_server_spawn_item_drop(mc_server_t *server, double x, double y, double z, const mc_slot_t *slot);
void net_server_close_container_viewers(mc_server_t *server, mc_container_kind_t kind, int32_t x, int32_t y, int32_t z);
int net_server_get_open_container_snapshot(mc_server_t *server, mc_container_kind_t kind, int32_t x, int32_t y, int32_t z,
                                           mc_container_instance_t *out);

#endif /* MC_SERVER_H */
