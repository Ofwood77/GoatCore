#ifndef MC_NET_H
#define MC_NET_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>
#include "mc_server.h"
#include "mc_inventory.h"
#include "mc_mining.h"
#include "mc_world.h"

#define MC_BUF_CAP 8192

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    size_t rpos;
} mc_buf_t;

typedef struct {
    int32_t cx;
    int32_t cz;
    uint32_t prio;
} mc_chunk_req_t;

typedef struct {
    int32_t entity_id;
    uint8_t uuid[16];
    bool has_last_pos;
    bool listed;
    bool spawned;
    double x;
    double y;
    double z;
    float yaw;
    float pitch;
    float head_yaw;
} mc_remote_player_t;

struct mc_conn {
    int fd;
    mc_buf_t in;
    mc_buf_t out;
    pthread_mutex_t out_lock;
    atomic_int state;
    atomic_bool closing;
    const mc_server_config_t *cfg;
    mc_server_t *server;
    atomic_int refcount;

    char username[17];
    uint8_t uuid[16];
    bool has_uuid;
    bool login_success_sent;
    bool config_known_packs_sent;
    bool config_registry_sent;
    bool config_finish_sent;
    bool play_init_sent;
    bool play_ready;

    int32_t entity_id;
    int32_t gamemode;
    float health;
    int32_t food;
    float food_saturation;
    float food_exhaustion;
    bool is_using_item;
    bool use_item_input_held;
    int32_t using_hand;
    int32_t using_slot;
    int32_t using_item_id;
    int32_t use_item_remaining_ticks;
    bool dead;
    int32_t teleport_id;
    mc_player_data_t *player;
    int64_t keepalive_id;
    int64_t last_keepalive_sent_ms;
    bool awaiting_keepalive;

    mc_mining_session_t mining;

    double x;
    double y;
    double z;
    float yaw;
    float pitch;
    bool has_pos;
    bool on_ground;
    bool fall_tracking;
    double fall_start_y;
    int64_t next_void_damage_ms;
    int64_t next_natural_regen_ms;
    int64_t next_starvation_damage_ms;

    bool encryption_enabled;
    void *ssl_read;  /* EVP_CIPHER_CTX* */
    void *ssl_write; /* EVP_CIPHER_CTX* */

    struct mc_conn *next;

    /* Chunk streaming state (PLAY) */
    bool has_center_chunk;
    int32_t center_cx;
    int32_t center_cz;
    int32_t next_window_id;
    bool chunk_refresh_ping_pending;
    int32_t chunk_refresh_ping_id;

    struct {
        int64_t *keys;
        uint8_t *states; /* 0 empty, 1 used, 2 tomb */
        size_t cap;
        size_t len;
        size_t tombs;
    } sent_chunks;

    struct {
        mc_chunk_req_t *items;
        size_t head;
        size_t len;
        size_t cap;
    } pending_chunks;

    mc_remote_player_t *remote_players;
    size_t remote_players_len;
    size_t remote_players_cap;

    mc_active_window_t active_window;
};

typedef struct {
    int32_t packet_id;
    mc_buf_t payload;
} mc_frame_t;

int conn_read_frame(mc_conn_t *c, mc_frame_t *out_frame, int compression_threshold);
int conn_write_packet(mc_conn_t *c, int32_t packet_id, const uint8_t *payload, size_t payload_len, int compression_threshold);
void conn_close(mc_conn_t *c);
mc_conn_t *net_server_find_conn_by_name(mc_server_t *server, const char *name);
void net_server_release_conn(mc_conn_t *conn);
mc_world_t *net_server_world(mc_server_t *server);

int buf_init(mc_buf_t *b, size_t cap);
void buf_free(mc_buf_t *b);
void buf_compact(mc_buf_t *b);
int buf_reserve(mc_buf_t *b, size_t need);
int buf_write(mc_buf_t *b, const uint8_t *src, size_t n);

#endif /* MC_NET_H */
