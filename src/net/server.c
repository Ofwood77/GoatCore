#include "mc_server.h"
#include "mc_net.h"
#include "mc_protocol.h"
#include "mc_task_queue.h"
#include "mc_util.h"
#include "generated_minecraft_ids.h"
#include "generated_registries.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <math.h>
#include <sys/epoll.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>

#define MAX_EVENTS 64
#define PKT_PLAY_BLOCK_UPDATE MC_PKT_PLAY_CLIENTBOUND_BLOCK_UPDATE
#define PKT_PLAY_SPAWN_ENTITY MC_PKT_PLAY_CLIENTBOUND_ADD_ENTITY
#define PKT_PLAY_ENTITY_METADATA MC_PKT_PLAY_CLIENTBOUND_SET_ENTITY_DATA
#define PKT_PLAY_ENTITY_DESTROY MC_PKT_PLAY_CLIENTBOUND_REMOVE_ENTITIES
#define PKT_PLAY_ENTITY_TELEPORT MC_PKT_PLAY_CLIENTBOUND_TELEPORT_ENTITY
#define WORLD_EVICT_PERIOD_TICKS 20
#define WORLD_EVICT_BUDGET 32
#define ITEM_ENTITY_TTL_MS 30000
#define ITEM_ENTITY_DEFAULT_PICKUP_DELAY_TICKS 10
#define ITEM_ENTITY_PICKUP_RADIUS 1.5
#define ITEM_ENTITY_PICKUP_RADIUS_SQ (ITEM_ENTITY_PICKUP_RADIUS * ITEM_ENTITY_PICKUP_RADIUS)
#define ITEM_ENTITY_GRAVITY_PER_TICK 0.08
#define ITEM_ENTITY_MAX_FALL_SPEED 3.92
#define ITEM_ENTITY_AIR_DRAG 0.98
#define ITEM_ENTITY_GROUND_DRAG 0.60
#define ITEM_ENTITY_MIN_HORIZONTAL_SPEED 0.001
#define ITEM_ENTITY_HALF_WIDTH 0.125
#define ITEM_ENTITY_HALF_HEIGHT 0.125
#define ITEM_ENTITY_COLLISION_EPSILON 0.001
#define ITEM_ENTITY_METADATA_SLOT_INDEX 8

typedef struct {
    int32_t entity_id;
    uint8_t uuid[16];
    double x;
    double y;
    double z;
    double vx;
    double vy;
    double vz;
    int64_t expires_at_ms;
    int32_t pickup_delay_ticks;
    mc_slot_t slot;
} mc_item_entity_t;

struct mc_server {
    int listen_fd;
    int epoll_fd;
    mc_server_config_t cfg;
    atomic_bool running;
    mc_conn_t *conns;
    int32_t next_entity_id;
    pthread_mutex_t conns_lock;
    mc_task_queue_t task_queue;
    pthread_t tick_thread;
    atomic_int difficulty;
    mc_world_t *world;
    mc_item_entity_t *item_entities;
    size_t item_entities_len;
    size_t item_entities_cap;
};

static mc_difficulty_t normalize_difficulty(mc_difficulty_t difficulty) {
    switch (difficulty) {
        case MC_DIFFICULTY_PEACEFUL:
        case MC_DIFFICULTY_EASY:
        case MC_DIFFICULTY_NORMAL:
        case MC_DIFFICULTY_HARD:
            return difficulty;
        default:
            return MC_DIFFICULTY_NORMAL;
    }
}

static const char *state_name(mc_proto_state_t state) {
    switch (state) {
        case MC_STATE_HANDSHAKING: return "HANDSHAKING";
        case MC_STATE_STATUS: return "STATUS";
        case MC_STATE_LOGIN: return "LOGIN";
        case MC_STATE_PLAY: return "PLAY";
        case MC_STATE_CONFIGURATION: return "CONFIG";
        default: return "UNKNOWN";
    }
}

static const char *serverbound_packet_name(mc_proto_state_t state, int32_t id) {
    return mc_minecraft_packet_name(state, MC_MINECRAFT_PACKET_DIR_SERVERBOUND, id);
}

static bool should_log_recv_packet(mc_proto_state_t state, int32_t id) {
    if (state == MC_STATE_PLAY) {
        if (id == MC_PKT_PLAY_SERVERBOUND_CLIENT_TICK_END) return false;
        if (id == MC_PKT_PLAY_SERVERBOUND_MOVE_PLAYER_POS) return false;
        if (id == MC_PKT_PLAY_SERVERBOUND_MOVE_PLAYER_POS_ROT) return false;
        if (id == MC_PKT_PLAY_SERVERBOUND_MOVE_PLAYER_ROT) return false;
        if (id == MC_PKT_PLAY_SERVERBOUND_PLAYER_INPUT) return false;
    }
    return true;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int add_epoll(int epfd, int fd, uint32_t events, void *ptr) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.ptr = ptr;
    return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

static void del_epoll(int epfd, int fd) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
}

static mc_conn_t *conn_create(mc_server_t *s, int fd) {
    mc_conn_t *c = (mc_conn_t *)calloc(1, sizeof(mc_conn_t));
    if (!c) return NULL;
    c->fd = fd;
    c->state = MC_STATE_HANDSHAKING;
    c->cfg = &s->cfg;
    c->server = s;
    c->entity_id = s->next_entity_id++;
    atomic_init(&c->refcount, 1);
    if (pthread_mutex_init(&c->out_lock, NULL) != 0) {
        free(c);
        return NULL;
    }
    if (buf_init(&c->in, MC_BUF_CAP) != 0 || buf_init(&c->out, MC_BUF_CAP) != 0) {
        buf_free(&c->in);
        buf_free(&c->out);
        pthread_mutex_destroy(&c->out_lock);
        free(c);
        return NULL;
    }
    pthread_mutex_lock(&s->conns_lock);
    c->next = s->conns;
    s->conns = c;
    pthread_mutex_unlock(&s->conns_lock);
    return c;
}

static void conn_destroy(mc_conn_t *c) {
    if (!c) return;
    proto_play_conn_cleanup(c);
    if (c->fd >= 0) close(c->fd);
    buf_free(&c->in);
    buf_free(&c->out);
    pthread_mutex_destroy(&c->out_lock);
    free(c->sent_chunks.keys);
    free(c->sent_chunks.states);
    free(c->pending_chunks.items);
    free(c->remote_players);
    free(c);
}

static void conn_remove(mc_server_t *s, mc_conn_t *c) {
    pthread_mutex_lock(&s->conns_lock);
    mc_conn_t **pp = &s->conns;
    while (*pp) {
        if (*pp == c) {
            *pp = c->next;
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&s->conns_lock);
}

static void conn_acquire(mc_conn_t *c) {
    if (!c) return;
    atomic_fetch_add(&c->refcount, 1);
}

static void conn_release(mc_conn_t *c) {
    if (!c) return;
    if (atomic_fetch_sub(&c->refcount, 1) == 1) {
        conn_destroy(c);
    }
}

typedef struct {
    mc_conn_t **items;
    size_t len;
    size_t cap;
} mc_conn_snapshot_t;

static void conn_snapshot_clear(mc_conn_snapshot_t *snapshot) {
    if (!snapshot) return;
    for (size_t i = 0; i < snapshot->len; i++) {
        conn_release(snapshot->items[i]);
    }
    free(snapshot->items);
    memset(snapshot, 0, sizeof(*snapshot));
}

static int conn_snapshot_push(mc_conn_snapshot_t *snapshot, mc_conn_t *conn) {
    if (!snapshot || !conn) return -1;
    if (snapshot->len == snapshot->cap) {
        size_t new_cap = snapshot->cap ? snapshot->cap * 2 : 16;
        if (new_cap < snapshot->cap) return -1;
        mc_conn_t **next = (mc_conn_t **)realloc(snapshot->items, new_cap * sizeof(*next));
        if (!next) return -1;
        snapshot->items = next;
        snapshot->cap = new_cap;
    }
    conn_acquire(conn);
    snapshot->items[snapshot->len++] = conn;
    return 0;
}

static int server_snapshot_conns(mc_server_t *s, mc_conn_snapshot_t *snapshot) {
    if (!s || !snapshot) return -1;
    memset(snapshot, 0, sizeof(*snapshot));
    pthread_mutex_lock(&s->conns_lock);
    for (mc_conn_t *c = s->conns; c; c = c->next) {
        if (conn_snapshot_push(snapshot, c) != 0) {
            pthread_mutex_unlock(&s->conns_lock);
            conn_snapshot_clear(snapshot);
            return -1;
        }
    }
    pthread_mutex_unlock(&s->conns_lock);
    return 0;
}

static void close_matching_container_window(mc_conn_t *c) {
    if (!c || !c->active_window.open || !c->active_window.container) return;
    int32_t window_id = c->active_window.window_id;
    uint8_t payload[8];
    size_t payload_len = 0;
    if (varint_write(payload, sizeof(payload), window_id, &payload_len) != 0) return;
    mc_container_instance_clear(c->active_window.container);
    free(c->active_window.container);
    memset(&c->active_window, 0, sizeof(c->active_window));
    (void)conn_write_packet(c, MC_PKT_PLAY_CLIENTBOUND_CONTAINER_CLOSE, payload, payload_len, -1);
}

static void server_close_conn(mc_server_t *s, mc_conn_t *c) {
    if (!s || !c) return;
    if (c->state == MC_STATE_PLAY) {
        pthread_mutex_lock(&s->conns_lock);
        for (mc_conn_t *other = s->conns; other; other = other->next) {
            if (other == c || other->closing || other->state != MC_STATE_PLAY) continue;
            if (proto_play_remove_remote_player(other, c) != 0) {
                conn_close(other);
            }
        }
        pthread_mutex_unlock(&s->conns_lock);
    }
    del_epoll(s->epoll_fd, c->fd);
    conn_remove(s, c);
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    c->closing = true;
    conn_release(c);
}

static int64_t now_ms(void) {
    return mc_now_us() / 1000;
}

static int32_t floor_i32_from_f64(double v) {
    int64_t i = (int64_t)v;
    if ((double)i > v) i--;
    if (i < INT32_MIN) return INT32_MIN;
    if (i > INT32_MAX) return INT32_MAX;
    return (int32_t)i;
}

static int32_t block_to_chunk(int32_t b) {
    return (b >= 0) ? (b / 16) : ((b - 15) / 16);
}

static bool block_state_is_passable(const mc_world_ids_t *ids, int32_t state_id) {
    if (!ids || state_id < 0) return false;
    if (state_id == ids->air) return true;
    for (int i = 0; i < 16; i++) {
        if (state_id == ids->water_level[i]) return true;
    }
    const char *key = mc_block_state_key(state_id);
    if (!key) return false;
    return strcmp(key, "minecraft:cave_air") == 0 ||
           strcmp(key, "minecraft:void_air") == 0 ||
           strcmp(key, "minecraft:short_grass") == 0 ||
           strncmp(key, "minecraft:tall_grass[", 21) == 0 ||
           strcmp(key, "minecraft:fern") == 0 ||
           strncmp(key, "minecraft:large_fern[", 21) == 0 ||
           strcmp(key, "minecraft:seagrass") == 0 ||
           strncmp(key, "minecraft:tall_seagrass[", 24) == 0 ||
           strcmp(key, "minecraft:dead_bush") == 0 ||
           strncmp(key, "minecraft:snow[", 15) == 0 ||
           strncmp(key, "minecraft:vine[", 15) == 0 ||
           strncmp(key, "minecraft:fire[", 15) == 0;
}

static int resolve_item_entity_type_id(void) {
    static int init = 0;
    static int value = -1;
    if (!init) {
        value = mc_minecraft_entity_type_id("minecraft:item");
        /* Local generated ids for the current asset set map minecraft:item -> 57.
         * Keep a hard fallback so block drops never silently degrade to a wrong entity type. */
        if (value < 0) value = 57;
        if (value < 0) log_error("failed to resolve entity type id for minecraft:item");
        init = 1;
    }
    return value;
}

static void fill_entity_uuid_from_id(int32_t entity_id, uint8_t out[16]) {
    memset(out, 0, 16);
    out[0] = 0x12;
    out[1] = 0x34;
    out[2] = 0x56;
    out[3] = 0x78;
    out[4] = (uint8_t)((entity_id >> 24) & 0xFF);
    out[5] = (uint8_t)((entity_id >> 16) & 0xFF);
    out[6] = (uint8_t)(((entity_id >> 8) & 0x0F) | 0x40);
    out[7] = (uint8_t)(entity_id & 0xFF);
    out[8] = 0x80;
    out[12] = (uint8_t)((entity_id >> 24) & 0xFF);
    out[13] = (uint8_t)((entity_id >> 16) & 0xFF);
    out[14] = (uint8_t)((entity_id >> 8) & 0xFF);
    out[15] = (uint8_t)(entity_id & 0xFF);
}

static int keep_keys_push(int64_t **arr, size_t *len, size_t *cap, int64_t key) {
    if (!arr || !len || !cap) return -1;
    if (*len == *cap) {
        size_t new_cap = *cap ? (*cap * 2) : 1024;
        if (new_cap < *cap) return -1;
        int64_t *next = (int64_t *)realloc(*arr, new_cap * sizeof(*next));
        if (!next) return -1;
        *arr = next;
        *cap = new_cap;
    }
    (*arr)[(*len)++] = key;
    return 0;
}

typedef struct {
    uint64_t count;
    int64_t total_us;
    int64_t max_us;
} perf_accum_t;

static void perf_accum_add(perf_accum_t *acc, int64_t us) {
    if (!acc) return;
    if (us < 0) us = 0;
    acc->count++;
    acc->total_us += us;
    if (us > acc->max_us) acc->max_us = us;
}

static double perf_accum_avg_ms(const perf_accum_t *acc) {
    if (!acc || acc->count == 0) return 0.0;
    return ((double)acc->total_us / (double)acc->count) / 1000.0;
}

static double perf_ms(int64_t us) {
    return (double)us / 1000.0;
}

int net_server_init(mc_server_t **out, const mc_server_config_t *cfg) {
    if (!out || !cfg) return -1;

    mc_server_t *s = (mc_server_t *)calloc(1, sizeof(mc_server_t));
    if (!s) return -1;
    s->cfg = *cfg;
    s->cfg.difficulty = normalize_difficulty(cfg->difficulty);
    s->next_entity_id = 1;
    s->conns = NULL;
    atomic_init(&s->running, false);
    atomic_init(&s->difficulty, (int)s->cfg.difficulty);
    if (pthread_mutex_init(&s->conns_lock, NULL) != 0) {
        free(s);
        return -1;
    }
    if (mc_task_queue_init(&s->task_queue) != 0) {
        pthread_mutex_destroy(&s->conns_lock);
        free(s);
        return -1;
    }

    s->world = mc_world_create(cfg->world_path, cfg->level_seed);
    if (!s->world) {
        mc_task_queue_destroy(&s->task_queue);
        pthread_mutex_destroy(&s->conns_lock);
        free(s);
        return -1;
    }

    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd < 0) {
        log_error("socket() failed for %s:%u (errno=%d %s)",
                  cfg->bind_ip ? cfg->bind_ip : "0.0.0.0",
                  (unsigned)cfg->bind_port,
                  errno,
                  strerror(errno));
        mc_world_destroy(s->world);
        mc_task_queue_destroy(&s->task_queue);
        pthread_mutex_destroy(&s->conns_lock);
        free(s);
        return -1;
    }

    int yes = 1;
    setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg->bind_port);
    addr.sin_addr.s_addr = cfg->bind_ip ? inet_addr(cfg->bind_ip) : INADDR_ANY;

    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        log_error("bind() failed for %s:%u (errno=%d %s)",
                  cfg->bind_ip ? cfg->bind_ip : "0.0.0.0",
                  (unsigned)cfg->bind_port,
                  errno,
                  strerror(errno));
        close(s->listen_fd);
        mc_world_destroy(s->world);
        mc_task_queue_destroy(&s->task_queue);
        pthread_mutex_destroy(&s->conns_lock);
        free(s);
        return -1;
    }

    if (listen(s->listen_fd, cfg->max_connections) != 0) {
        log_error("listen() failed for %s:%u (errno=%d %s)",
                  cfg->bind_ip ? cfg->bind_ip : "0.0.0.0",
                  (unsigned)cfg->bind_port,
                  errno,
                  strerror(errno));
        close(s->listen_fd);
        mc_world_destroy(s->world);
        mc_task_queue_destroy(&s->task_queue);
        pthread_mutex_destroy(&s->conns_lock);
        free(s);
        return -1;
    }

    if (set_nonblocking(s->listen_fd) != 0) {
        log_error("failed to set listening socket non-blocking for %s:%u (errno=%d %s)",
                  cfg->bind_ip ? cfg->bind_ip : "0.0.0.0",
                  (unsigned)cfg->bind_port,
                  errno,
                  strerror(errno));
        close(s->listen_fd);
        mc_world_destroy(s->world);
        mc_task_queue_destroy(&s->task_queue);
        pthread_mutex_destroy(&s->conns_lock);
        free(s);
        return -1;
    }

    s->epoll_fd = epoll_create1(0);
    if (s->epoll_fd < 0) {
        log_error("epoll_create1() failed (errno=%d %s)", errno, strerror(errno));
        close(s->listen_fd);
        mc_world_destroy(s->world);
        mc_task_queue_destroy(&s->task_queue);
        pthread_mutex_destroy(&s->conns_lock);
        free(s);
        return -1;
    }

    if (add_epoll(s->epoll_fd, s->listen_fd, EPOLLIN, s) != 0) {
        log_error("failed to register listening socket in epoll for %s:%u (errno=%d %s)",
                  cfg->bind_ip ? cfg->bind_ip : "0.0.0.0",
                  (unsigned)cfg->bind_port,
                  errno,
                  strerror(errno));
        close(s->epoll_fd);
        close(s->listen_fd);
        mc_world_destroy(s->world);
        mc_task_queue_destroy(&s->task_queue);
        pthread_mutex_destroy(&s->conns_lock);
        free(s);
        return -1;
    }

    *out = s;
    return 0;
}

static int w_u64(uint8_t *buf, size_t cap, size_t *pos, uint64_t v) {
    if (!buf || !pos) return -1;
    if (*pos + 8 > cap) return -1;
    buf[*pos + 0] = (uint8_t)((v >> 56) & 0xFF);
    buf[*pos + 1] = (uint8_t)((v >> 48) & 0xFF);
    buf[*pos + 2] = (uint8_t)((v >> 40) & 0xFF);
    buf[*pos + 3] = (uint8_t)((v >> 32) & 0xFF);
    buf[*pos + 4] = (uint8_t)((v >> 24) & 0xFF);
    buf[*pos + 5] = (uint8_t)((v >> 16) & 0xFF);
    buf[*pos + 6] = (uint8_t)((v >> 8) & 0xFF);
    buf[*pos + 7] = (uint8_t)(v & 0xFF);
    *pos += 8;
    return 0;
}

static int w_u32(uint8_t *buf, size_t cap, size_t *pos, uint32_t v) {
    if (!buf || !pos || *pos + 4 > cap) return -1;
    buf[*pos + 0] = (uint8_t)((v >> 24) & 0xFF);
    buf[*pos + 1] = (uint8_t)((v >> 16) & 0xFF);
    buf[*pos + 2] = (uint8_t)((v >> 8) & 0xFF);
    buf[*pos + 3] = (uint8_t)(v & 0xFF);
    *pos += 4;
    return 0;
}

static int w_i32(uint8_t *buf, size_t cap, size_t *pos, int32_t v) {
    return w_u32(buf, cap, pos, (uint32_t)v);
}

static int w_byte(uint8_t *buf, size_t cap, size_t *pos, int8_t v) {
    if (!buf || !pos || *pos + 1 > cap) return -1;
    buf[*pos] = (uint8_t)v;
    *pos += 1;
    return 0;
}

static int w_ubyte(uint8_t *buf, size_t cap, size_t *pos, uint8_t v) {
    if (!buf || !pos || *pos + 1 > cap) return -1;
    buf[*pos] = v;
    *pos += 1;
    return 0;
}

static int w_u16_be(uint8_t *buf, size_t cap, size_t *pos, uint16_t v) {
    if (!buf || !pos || *pos + 2 > cap) return -1;
    buf[*pos] = (uint8_t)((v >> 8) & 0xFF);
    buf[*pos + 1] = (uint8_t)(v & 0xFF);
    *pos += 2;
    return 0;
}

static int w_f64(uint8_t *buf, size_t cap, size_t *pos, double v) {
    uint64_t u = 0;
    memcpy(&u, &v, sizeof(u));
    return w_u64(buf, cap, pos, u);
}

static int w_f32(uint8_t *buf, size_t cap, size_t *pos, float v) {
    uint32_t u = 0;
    memcpy(&u, &v, sizeof(u));
    return w_u32(buf, cap, pos, u);
}

static int w_position(uint8_t *buf, size_t cap, size_t *pos, int32_t x, int32_t y, int32_t z) {
    uint64_t packed = ((uint64_t)(x & 0x3FFFFFF) << 38) | ((uint64_t)(z & 0x3FFFFFF) << 12) | ((uint64_t)(y & 0xFFF));
    return w_u64(buf, cap, pos, packed);
}

static int w_varint(uint8_t *buf, size_t cap, size_t *pos, int32_t v) {
    size_t n = 0;
    if (varint_write(buf + *pos, cap - *pos, v, &n) != 0) return -1;
    *pos += n;
    return 0;
}

static int w_bool(uint8_t *buf, size_t cap, size_t *pos, bool v) {
    return w_ubyte(buf, cap, pos, v ? 1 : 0);
}

static int w_lpvec3(uint8_t *buf, size_t cap, size_t *pos, double x, double y, double z) {
    double ax = fabs(x);
    double ay = fabs(y);
    double az = fabs(z);
    double abs_max = ax;
    if (ay > abs_max) abs_max = ay;
    if (az > abs_max) abs_max = az;

    if (abs_max < 3.051944088384301e-5) {
        return w_ubyte(buf, cap, pos, 0);
    }

    return -1;
}

static int send_block_update(mc_conn_t *c, int32_t x, int32_t y, int32_t z, int32_t block_state) {
    if (!c) return -1;
    uint8_t buf[32];
    size_t pos = 0;
    if (w_position(buf, sizeof(buf), &pos, x, y, z) != 0) return -1;
    if (w_varint(buf, sizeof(buf), &pos, block_state) != 0) return -1;
    return conn_write_packet(c, PKT_PLAY_BLOCK_UPDATE, buf, pos, -1);
}

static int send_item_entity_spawn(mc_conn_t *c, const mc_item_entity_t *item) {
    if (!c || !item) return -1;
    uint8_t buf[160];
    size_t pos = 0;
    int item_entity_type_id = resolve_item_entity_type_id();
    if (item_entity_type_id < 0) return -1;
    if (w_varint(buf, sizeof(buf), &pos, item->entity_id) != 0) return -1;
    if (pos + 16 > sizeof(buf)) return -1;
    memcpy(buf + pos, item->uuid, 16);
    pos += 16;
    if (w_varint(buf, sizeof(buf), &pos, item_entity_type_id) != 0) return -1;
    if (w_f64(buf, sizeof(buf), &pos, item->x) != 0 || w_f64(buf, sizeof(buf), &pos, item->y) != 0 ||
        w_f64(buf, sizeof(buf), &pos, item->z) != 0) {
        return -1;
    }
    if (w_lpvec3(buf, sizeof(buf), &pos, 0.0, 0.0, 0.0) != 0) return -1;
    if (w_byte(buf, sizeof(buf), &pos, 0) != 0 || w_byte(buf, sizeof(buf), &pos, 0) != 0 || w_byte(buf, sizeof(buf), &pos, 0) != 0) {
        return -1;
    }
    if (w_varint(buf, sizeof(buf), &pos, 0) != 0) return -1;
    return conn_write_packet(c, PKT_PLAY_SPAWN_ENTITY, buf, pos, -1);
}

static int send_item_entity_metadata(mc_conn_t *c, const mc_item_entity_t *item) {
    if (!c || !item) return -1;
    size_t slot_cap = item->slot.components_len + 256;
    if (slot_cap < 64) slot_cap = 64;
    uint8_t *raw_slot = (uint8_t *)malloc(slot_cap);
    if (!raw_slot) return -1;
    size_t slot_pos = 0;
    if (mc_slot_write_net(raw_slot, slot_cap, &slot_pos, &item->slot) != 0) {
        free(raw_slot);
        return -1;
    }
    size_t buf_cap = slot_pos + 32;
    uint8_t *buf = (uint8_t *)malloc(buf_cap);
    if (!buf) {
        free(raw_slot);
        return -1;
    }
    size_t pos = 0;
    if (w_varint(buf, buf_cap, &pos, item->entity_id) != 0 || w_ubyte(buf, buf_cap, &pos, ITEM_ENTITY_METADATA_SLOT_INDEX) != 0 ||
        w_varint(buf, buf_cap, &pos, 7) != 0) {
        free(buf);
        free(raw_slot);
        return -1;
    }
    if (pos + slot_pos + 1 > buf_cap) {
        free(buf);
        free(raw_slot);
        return -1;
    }
    memcpy(buf + pos, raw_slot, slot_pos);
    pos += slot_pos;
    buf[pos++] = 0xFF;
    int rc = conn_write_packet(c, PKT_PLAY_ENTITY_METADATA, buf, pos, -1);
    free(buf);
    free(raw_slot);
    return rc;
}

static int send_item_entity_teleport(mc_conn_t *c, const mc_item_entity_t *item) {
    if (!c || !item) return -1;
    uint8_t buf[96];
    size_t pos = 0;
    if (w_varint(buf, sizeof(buf), &pos, item->entity_id) != 0) return -1;
    if (w_f64(buf, sizeof(buf), &pos, item->x) != 0 || w_f64(buf, sizeof(buf), &pos, item->y) != 0 ||
        w_f64(buf, sizeof(buf), &pos, item->z) != 0) {
        return -1;
    }
    if (w_f64(buf, sizeof(buf), &pos, 0.0) != 0 || w_f64(buf, sizeof(buf), &pos, 0.0) != 0 ||
        w_f64(buf, sizeof(buf), &pos, 0.0) != 0) {
        return -1;
    }
    if (w_f32(buf, sizeof(buf), &pos, 0.0f) != 0 || w_f32(buf, sizeof(buf), &pos, 0.0f) != 0 ||
        w_i32(buf, sizeof(buf), &pos, 0) != 0 ||
        w_bool(buf, sizeof(buf), &pos, item->vy == 0.0) != 0) {
        return -1;
    }
    return conn_write_packet(c, PKT_PLAY_ENTITY_TELEPORT, buf, pos, -1);
}

static int broadcast_item_entity_teleport(mc_server_t *s, const mc_item_entity_t *item) {
    if (!s || !item) return -1;
    int rc = 0;
    for (mc_conn_t *c = s->conns; c; c = c->next) {
        if (c->closing || c->state != MC_STATE_PLAY || !c->play_ready) continue;
        if (send_item_entity_teleport(c, item) != 0) {
            conn_close(c);
            rc = -1;
        }
    }
    return rc;
}

static int send_item_entity_destroy(mc_conn_t *c, int32_t entity_id) {
    if (!c) return -1;
    uint8_t buf[16];
    size_t pos = 0;
    if (w_varint(buf, sizeof(buf), &pos, 1) != 0 || w_varint(buf, sizeof(buf), &pos, entity_id) != 0) return -1;
    return conn_write_packet(c, PKT_PLAY_ENTITY_DESTROY, buf, pos, -1);
}

static void destroy_item_entity_at(mc_server_t *s, size_t idx) {
    if (!s || idx >= s->item_entities_len) return;
    int32_t entity_id = s->item_entities[idx].entity_id;
    for (mc_conn_t *c = s->conns; c; c = c->next) {
        if (c->closing || c->state != MC_STATE_PLAY || !c->play_ready) continue;
        if (send_item_entity_destroy(c, entity_id) != 0) conn_close(c);
    }
    mc_slot_clear(&s->item_entities[idx].slot);
    size_t last = s->item_entities_len - 1;
    if (idx != last) s->item_entities[idx] = s->item_entities[last];
    s->item_entities_len--;
}

static bool item_entity_intersects_block_at(const mc_item_entity_t *item, int32_t bx, int32_t by, int32_t bz) {
    if (!item) return false;
    double min_x = item->x - ITEM_ENTITY_HALF_WIDTH;
    double max_x = item->x + ITEM_ENTITY_HALF_WIDTH;
    double min_y = item->y - ITEM_ENTITY_HALF_HEIGHT;
    double max_y = item->y + ITEM_ENTITY_HALF_HEIGHT;
    double min_z = item->z - ITEM_ENTITY_HALF_WIDTH;
    double max_z = item->z + ITEM_ENTITY_HALF_WIDTH;
    return max_x > (double)bx + ITEM_ENTITY_COLLISION_EPSILON &&
           min_x < (double)bx + 1.0 - ITEM_ENTITY_COLLISION_EPSILON &&
           max_y > (double)by + ITEM_ENTITY_COLLISION_EPSILON &&
           min_y < (double)by + 1.0 - ITEM_ENTITY_COLLISION_EPSILON &&
           max_z > (double)bz + ITEM_ENTITY_COLLISION_EPSILON &&
           min_z < (double)bz + 1.0 - ITEM_ENTITY_COLLISION_EPSILON;
}

static bool item_entity_position_is_free(mc_world_t *world, const mc_world_ids_t *ids, double x, double y, double z) {
    if (!world || !ids) return false;
    double min_x = x - ITEM_ENTITY_HALF_WIDTH + ITEM_ENTITY_COLLISION_EPSILON;
    double max_x = x + ITEM_ENTITY_HALF_WIDTH - ITEM_ENTITY_COLLISION_EPSILON;
    double min_y = y - ITEM_ENTITY_HALF_HEIGHT + ITEM_ENTITY_COLLISION_EPSILON;
    double max_y = y + ITEM_ENTITY_HALF_HEIGHT - ITEM_ENTITY_COLLISION_EPSILON;
    double min_z = z - ITEM_ENTITY_HALF_WIDTH + ITEM_ENTITY_COLLISION_EPSILON;
    double max_z = z + ITEM_ENTITY_HALF_WIDTH - ITEM_ENTITY_COLLISION_EPSILON;
    int32_t min_bx = floor_i32_from_f64(min_x);
    int32_t max_bx = floor_i32_from_f64(max_x);
    int32_t min_by = floor_i32_from_f64(min_y);
    int32_t max_by = floor_i32_from_f64(max_y);
    int32_t min_bz = floor_i32_from_f64(min_z);
    int32_t max_bz = floor_i32_from_f64(max_z);

    for (int32_t by = min_by; by <= max_by; by++) {
        for (int32_t bz = min_bz; bz <= max_bz; bz++) {
            for (int32_t bx = min_bx; bx <= max_bx; bx++) {
                int32_t state_id = -1;
                if (mc_world_get_block(world, bx, by, bz, &state_id) != 0) return false;
                if (!block_state_is_passable(ids, state_id)) return false;
            }
        }
    }
    return true;
}

static void item_entity_consider_relocation_candidate(mc_world_t *world, const mc_world_ids_t *ids, const mc_item_entity_t *item,
                                                      double x, double y, double z, bool *found, double *best_dist_sq,
                                                      double *best_x, double *best_y, double *best_z) {
    if (!world || !ids || !item || !found || !best_dist_sq || !best_x || !best_y || !best_z) return;
    if (!item_entity_position_is_free(world, ids, x, y, z)) return;
    double dx = x - item->x;
    double dy = y - item->y;
    double dz = z - item->z;
    double dist_sq = dx * dx + dy * dy + dz * dz;
    if (!*found || dist_sq < *best_dist_sq) {
        *found = true;
        *best_dist_sq = dist_sq;
        *best_x = x;
        *best_y = y;
        *best_z = z;
    }
}

static bool relocate_item_entity_out_of_block(mc_server_t *s, mc_item_entity_t *item, int32_t bx, int32_t by, int32_t bz) {
    if (!s || !s->world || !item) return false;
    const mc_world_ids_t *ids = mc_world_ids(s->world);
    if (!ids || !item_entity_intersects_block_at(item, bx, by, bz)) return false;

    bool found = false;
    double best_dist_sq = 0.0;
    double best_x = item->x;
    double best_y = item->y;
    double best_z = item->z;
    const double eps = ITEM_ENTITY_COLLISION_EPSILON;

    item_entity_consider_relocation_candidate(s->world, ids, item,
                                              (double)bx - ITEM_ENTITY_HALF_WIDTH - eps, item->y, item->z,
                                              &found, &best_dist_sq, &best_x, &best_y, &best_z);
    item_entity_consider_relocation_candidate(s->world, ids, item,
                                              (double)bx + 1.0 + ITEM_ENTITY_HALF_WIDTH + eps, item->y, item->z,
                                              &found, &best_dist_sq, &best_x, &best_y, &best_z);
    item_entity_consider_relocation_candidate(s->world, ids, item,
                                              item->x, item->y, (double)bz - ITEM_ENTITY_HALF_WIDTH - eps,
                                              &found, &best_dist_sq, &best_x, &best_y, &best_z);
    item_entity_consider_relocation_candidate(s->world, ids, item,
                                              item->x, item->y, (double)bz + 1.0 + ITEM_ENTITY_HALF_WIDTH + eps,
                                              &found, &best_dist_sq, &best_x, &best_y, &best_z);
    item_entity_consider_relocation_candidate(s->world, ids, item,
                                              item->x, (double)by + 1.0 + ITEM_ENTITY_HALF_HEIGHT + eps, item->z,
                                              &found, &best_dist_sq, &best_x, &best_y, &best_z);

    for (int32_t oy = 1; oy <= 2 && !found; oy++) {
        double y = (double)by + (double)oy + ITEM_ENTITY_HALF_HEIGHT + eps;
        for (int32_t dz = -1; dz <= 1; dz++) {
            for (int32_t dx = -1; dx <= 1; dx++) {
                item_entity_consider_relocation_candidate(s->world, ids, item,
                                                          (double)bx + 0.5 + (double)dx, y, (double)bz + 0.5 + (double)dz,
                                                          &found, &best_dist_sq, &best_x, &best_y, &best_z);
            }
        }
    }

    if (!found) return false;

    item->x = best_x;
    item->y = best_y;
    item->z = best_z;
    item->vx = 0.0;
    item->vy = 0.0;
    item->vz = 0.0;
    return true;
}

static bool find_intersecting_solid_block_for_item(mc_world_t *world, const mc_world_ids_t *ids, const mc_item_entity_t *item,
                                                   int32_t *out_x, int32_t *out_y, int32_t *out_z) {
    if (!world || !ids || !item) return false;
    double min_x = item->x - ITEM_ENTITY_HALF_WIDTH + ITEM_ENTITY_COLLISION_EPSILON;
    double max_x = item->x + ITEM_ENTITY_HALF_WIDTH - ITEM_ENTITY_COLLISION_EPSILON;
    double min_y = item->y - ITEM_ENTITY_HALF_HEIGHT + ITEM_ENTITY_COLLISION_EPSILON;
    double max_y = item->y + ITEM_ENTITY_HALF_HEIGHT - ITEM_ENTITY_COLLISION_EPSILON;
    double min_z = item->z - ITEM_ENTITY_HALF_WIDTH + ITEM_ENTITY_COLLISION_EPSILON;
    double max_z = item->z + ITEM_ENTITY_HALF_WIDTH - ITEM_ENTITY_COLLISION_EPSILON;
    int32_t min_bx = floor_i32_from_f64(min_x);
    int32_t max_bx = floor_i32_from_f64(max_x);
    int32_t min_by = floor_i32_from_f64(min_y);
    int32_t max_by = floor_i32_from_f64(max_y);
    int32_t min_bz = floor_i32_from_f64(min_z);
    int32_t max_bz = floor_i32_from_f64(max_z);

    for (int32_t by = min_by; by <= max_by; by++) {
        for (int32_t bz = min_bz; bz <= max_bz; bz++) {
            for (int32_t bx = min_bx; bx <= max_bx; bx++) {
                int32_t state_id = -1;
                if (mc_world_get_block(world, bx, by, bz, &state_id) != 0) continue;
                if (block_state_is_passable(ids, state_id)) continue;
                if (out_x) *out_x = bx;
                if (out_y) *out_y = by;
                if (out_z) *out_z = bz;
                return true;
            }
        }
    }
    return false;
}

static bool resolve_item_entity_if_inside_solid(mc_server_t *s, mc_item_entity_t *item) {
    if (!s || !s->world || !item) return false;
    const mc_world_ids_t *ids = mc_world_ids(s->world);
    if (!ids) return false;
    int32_t bx = 0;
    int32_t by = 0;
    int32_t bz = 0;
    if (!find_intersecting_solid_block_for_item(s->world, ids, item, &bx, &by, &bz)) return false;
    return relocate_item_entity_out_of_block(s, item, bx, by, bz);
}

static void item_entities_tick(mc_server_t *s, int64_t now) {
    if (!s) return;
    size_t idx = 0;
    while (idx < s->item_entities_len) {
        mc_item_entity_t *item = &s->item_entities[idx];
        if (item->expires_at_ms <= now) {
            destroy_item_entity_at(s, idx);
            continue;
        }

        if (item->pickup_delay_ticks > 0) item->pickup_delay_ticks--;

        bool moved = false;
        bool metadata_dirty = false;
        bool on_ground = false;
        if (resolve_item_entity_if_inside_solid(s, item)) moved = true;

        double next_x = item->x + item->vx;
        double next_y = item->y;
        double next_z = item->z + item->vz;
        if (s->world) {
            item->vy -= ITEM_ENTITY_GRAVITY_PER_TICK;
            if (item->vy < -ITEM_ENTITY_MAX_FALL_SPEED) item->vy = -ITEM_ENTITY_MAX_FALL_SPEED;
            next_y = item->y + item->vy;
            if (item->vy < 0.0) {
                const mc_world_ids_t *ids = mc_world_ids(s->world);
                int32_t block_x = floor_i32_from_f64(item->x);
                int32_t block_z = floor_i32_from_f64(item->z);
                double next_bottom = next_y - ITEM_ENTITY_HALF_HEIGHT;
                int32_t block_y = floor_i32_from_f64(next_bottom);
                int32_t state_id = -1;
                if (mc_world_get_block(s->world, block_x, block_y, block_z, &state_id) == 0 && !block_state_is_passable(ids, state_id)) {
                    next_y = (double)block_y + 1.0 + ITEM_ENTITY_HALF_HEIGHT;
                    item->vy = 0.0;
                    on_ground = true;
                }
            }
            if (next_y < (double)MC_WORLD_MIN_Y + ITEM_ENTITY_HALF_HEIGHT) {
                next_y = (double)MC_WORLD_MIN_Y + ITEM_ENTITY_HALF_HEIGHT;
                item->vy = 0.0;
                on_ground = true;
            }
        }

        if (on_ground) {
            item->vx *= ITEM_ENTITY_GROUND_DRAG;
            item->vz *= ITEM_ENTITY_GROUND_DRAG;
        } else {
            item->vx *= ITEM_ENTITY_AIR_DRAG;
            item->vz *= ITEM_ENTITY_AIR_DRAG;
        }
        if (fabs(item->vx) < ITEM_ENTITY_MIN_HORIZONTAL_SPEED) item->vx = 0.0;
        if (fabs(item->vz) < ITEM_ENTITY_MIN_HORIZONTAL_SPEED) item->vz = 0.0;

        if (fabs(next_x - item->x) > 0.0001) {
            item->x = next_x;
            moved = true;
        }
        if (fabs(next_y - item->y) > 0.0001) {
            item->y = next_y;
            moved = true;
        }
        if (fabs(next_z - item->z) > 0.0001) {
            item->z = next_z;
            moved = true;
        }

        if (moved) {
            (void)broadcast_item_entity_teleport(s, item);
        }

        bool destroyed = false;
        if (item->pickup_delay_ticks <= 0) {
            for (mc_conn_t *c = s->conns; c; c = c->next) {
                if (c->closing || c->state != MC_STATE_PLAY || !c->play_ready || !c->has_pos) continue;
                double dx = c->x - item->x;
                double dy = c->y - item->y;
                double dz = c->z - item->z;
                double dist_sq = dx * dx + dy * dy + dz * dz;
                if (dist_sq > ITEM_ENTITY_PICKUP_RADIUS_SQ) continue;

                int pickup_rc = proto_play_try_pickup_ground_slot(c, &item->slot);
                if (pickup_rc < 0) {
                    conn_close(c);
                    continue;
                }
                if (pickup_rc == 0) continue;
                if (!item->slot.present || item->slot.count <= 0) {
                    destroy_item_entity_at(s, idx);
                    destroyed = true;
                    break;
                }
                metadata_dirty = true;
            }
        }
        if (destroyed) continue;

        if (metadata_dirty) {
            for (mc_conn_t *c = s->conns; c; c = c->next) {
                if (c->closing || c->state != MC_STATE_PLAY || !c->play_ready) continue;
                if (send_item_entity_metadata(c, item) != 0) conn_close(c);
            }
        }
        idx++;
    }
}

static int handle_client_read(mc_server_t *s, mc_conn_t *c) {
    uint8_t buf[4096];
    for (;;) {
        ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
        if (n == 0) return -1;
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return -1;
        }
        if (buf_write(&c->in, buf, (size_t)n) != 0) return -1;
    }

    mc_frame_t frame;
    memset(&frame, 0, sizeof(frame));

    for (;;) {
        int rc = conn_read_frame(c, &frame, s->cfg.compression_threshold);
        if (rc == 1) break;
        if (rc != 0) {
            log_error("frame decode error (state=%s)", state_name(c->state));
            return -1;
        }

        if (s->cfg.debug_packets) {
            const char *packet_name = serverbound_packet_name(c->state, frame.packet_id);
            if (should_log_recv_packet(c->state, frame.packet_id)) {
                if (packet_name) log_info("recv state=%s id=0x%02X (%s) len=%zu", state_name(c->state), frame.packet_id, packet_name, frame.payload.len);
                else log_info("recv state=%s id=0x%02X len=%zu", state_name(c->state), frame.packet_id, frame.payload.len);
            }
        }
        mc_task_t *task = (mc_task_t *)calloc(1, sizeof(mc_task_t));
        if (!task) {
            free(frame.payload.data);
            return -1;
        }
        task->type = MC_TASK_PACKET;
        task->conn = c;
        task->packet_id = frame.packet_id;
        task->payload = frame.payload.data;
        task->payload_len = frame.payload.len;
        task->enqueue_ms = now_ms();
        conn_acquire(c);
        mc_task_queue_push(&s->task_queue, task);
        frame.payload.data = NULL;
        frame.payload.len = 0;
        memset(&frame, 0, sizeof(frame));
    }

    return 0;
}

static int handle_client_write(mc_conn_t *c) {
    pthread_mutex_lock(&c->out_lock);
    while (c->out.rpos < c->out.len) {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        ssize_t n = send(c->fd, c->out.data + c->out.rpos, c->out.len - c->out.rpos, flags);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            pthread_mutex_unlock(&c->out_lock);
            return -1;
        }
        c->out.rpos += (size_t)n;
    }
    buf_compact(&c->out);
    if (c->closing && c->out.len == 0) {
        pthread_mutex_unlock(&c->out_lock);
        return -1;
    }
    pthread_mutex_unlock(&c->out_lock);
    return 0;
}

static int process_task(mc_server_t *s, mc_task_t *task, int64_t now_ms) {
    (void)s;
    if (!task) return 0;
    mc_conn_t *c = task->conn;
    if (!c) return 0;
    if (c->closing) return 0;

    mc_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.packet_id = task->packet_id;
    frame.payload.data = task->payload;
    frame.payload.len = task->payload_len;
    frame.payload.cap = task->payload_len;
    frame.payload.rpos = 0;

    int rc = 0;
    if (c->state == MC_STATE_HANDSHAKING) {
        if (frame.packet_id != MC_PKT_HANDSHAKING_CLIENT_INTENTION) {
            log_error("unexpected packet in HANDSHAKING: id=0x%02X", frame.packet_id);
            conn_close(c);
            rc = -1;
        } else if (proto_handle_handshake(c, &frame) != 0) {
            log_error("handler error (state=HANDSHAKING id=0x%02X)", frame.packet_id);
            conn_close(c);
            rc = -1;
        }
    } else if (c->state == MC_STATE_STATUS) {
        if (proto_handle_status(c, &frame, "C server stub", 0, 100) != 0) {
            log_error("handler error (state=STATUS id=0x%02X)", frame.packet_id);
            conn_close(c);
            rc = -1;
        }
    } else if (c->state == MC_STATE_LOGIN) {
        if (proto_handle_login(c, &frame) != 0) {
            log_error("handler error (state=LOGIN id=0x%02X)", frame.packet_id);
            conn_close(c);
            rc = -1;
        }
    } else if (c->state == MC_STATE_CONFIGURATION) {
        if (proto_config_handle(c, &frame) != 0) {
            log_error("handler error (state=CONFIG id=0x%02X)", frame.packet_id);
            conn_close(c);
            rc = -1;
        }
    } else if (c->state == MC_STATE_PLAY) {
        int64_t packet_now_ms = task->enqueue_ms > 0 ? task->enqueue_ms : now_ms;
        if (proto_play_handle(c, &frame, packet_now_ms) != 0) {
            log_error("handler error (state=PLAY id=0x%02X)", frame.packet_id);
            conn_close(c);
            rc = -1;
        }
    } else {
        log_error("invalid connection state=%d", c->state);
        conn_close(c);
        rc = -1;
    }

    return rc;
}

static bool server_container_is_open_unlocked(void *ctx, mc_container_kind_t kind, int32_t x, int32_t y, int32_t z) {
    mc_server_t *server = (mc_server_t *)ctx;
    if (!server || kind == MC_CONTAINER_KIND_NONE) return false;
    for (mc_conn_t *c = server->conns; c; c = c->next) {
        if (c->closing || !c->active_window.open || !c->active_window.container) continue;
        mc_container_instance_t *container = c->active_window.container;
        if (container->kind == kind && container->x == x && container->y == y && container->z == z) return true;
    }
    return false;
}

static void *tick_thread_main(void *arg) {
    mc_server_t *s = (mc_server_t *)arg;
    const int64_t tick_us = 50000;
    int64_t next_tick_us = mc_now_us();
    uint64_t tick_index = 0;
    perf_accum_t summary_total = {0};
    perf_accum_t summary_late = {0};
    perf_accum_t summary_task = {0};
    perf_accum_t summary_world = {0};
    perf_accum_t summary_furnace = {0};
    perf_accum_t summary_updates = {0};
    perf_accum_t summary_proto = {0};
    perf_accum_t summary_items = {0};
    perf_accum_t summary_remote = {0};
    perf_accum_t summary_write = {0};
    perf_accum_t summary_evict = {0};
    uint64_t summary_task_count = 0;
    uint64_t summary_update_sends = 0;
    uint64_t summary_proto_ticks = 0;
    uint64_t summary_remote_syncs = 0;
    uint64_t summary_resyncs = 0;

    while (atomic_load(&s->running)) {
        bool perf = mc_perf_enabled();
        int64_t tick_start_us = 0;
        int64_t tick_late_us = 0;
        int64_t sleep_planned_us = 0;
        int64_t task_wait_sum_us = 0;
        int64_t task_wait_max_us = 0;
        int64_t task_us = 0;
        int64_t world_tick_us = 0;
        int64_t furnace_tick_us = 0;
        int64_t update_broadcast_us = 0;
        int64_t proto_tick_us = 0;
        int64_t item_entities_us = 0;
        int64_t remote_sync_us = 0;
        int64_t write_flush_us = 0;
        int64_t evict_us = 0;
        int64_t clear_updates_us = 0;
        size_t task_count = 0;
        size_t conn_count = 0;
        size_t play_ready_count = 0;
        size_t block_update_sends = 0;
        size_t proto_tick_count = 0;
        size_t remote_sync_attempts = 0;
        size_t write_flush_count = 0;
        size_t evicted_count = 0;
        bool schedule_resync = false;
        mc_world_tick_stats_t world_stats = {0};
        mc_world_furnace_tick_stats_t furnace_stats = {0};

        int64_t now_us_value = mc_now_us();
        if (now_us_value < next_tick_us) {
            struct timespec ts;
            sleep_planned_us = next_tick_us - now_us_value;
            ts.tv_sec = sleep_planned_us / 1000000;
            ts.tv_nsec = (sleep_planned_us % 1000000) * 1000;
            nanosleep(&ts, NULL);
            now_us_value = mc_now_us();
        }
        if (now_us_value > next_tick_us) tick_late_us = now_us_value - next_tick_us;

        int64_t now = now_us_value / 1000;

        if (perf) tick_start_us = now_us_value;
        int64_t span_start_us = perf ? mc_now_us() : 0;
        mc_task_t *task = mc_task_queue_drain(&s->task_queue);
        while (task) {
            mc_task_t *next = task->next;
            task_count++;
            if (perf && task->enqueue_ms > 0) {
                int64_t wait_us = (now - task->enqueue_ms) * 1000;
                if (wait_us < 0) wait_us = 0;
                task_wait_sum_us += wait_us;
                if (wait_us > task_wait_max_us) task_wait_max_us = wait_us;
            }
            if (task->type == MC_TASK_PACKET) {
                (void)process_task(s, task, now);
            }
            if (task->payload) free(task->payload);
            if (task->conn) conn_release(task->conn);
            free(task);
            task = next;
        }
        if (perf) task_us = mc_now_us() - span_start_us;

        if (s->world) {
            span_start_us = perf ? mc_now_us() : 0;
            if (perf) mc_world_tick_profiled(s->world, now, &world_stats);
            else mc_world_tick(s->world, now);
            if (perf) world_tick_us = mc_now_us() - span_start_us;

            pthread_mutex_lock(&s->conns_lock);
            span_start_us = perf ? mc_now_us() : 0;
            if (perf) (void)mc_world_tick_furnaces_profiled(s->world, server_container_is_open_unlocked, s, &furnace_stats);
            else (void)mc_world_tick_furnaces(s->world, server_container_is_open_unlocked, s);
            if (perf) furnace_tick_us = mc_now_us() - span_start_us;
            pthread_mutex_unlock(&s->conns_lock);
        }

        const mc_block_update_t *updates = NULL;
        size_t updates_len = 0;
        if (s->world) {
            updates = mc_world_updates(s->world, &updates_len);
        }

        bool do_evict = (s->world != NULL) && ((tick_index % WORLD_EVICT_PERIOD_TICKS) == 0);
        int sim = s->cfg.simulation_distance;
        if (sim < 2) sim = 2;
        if (sim > 32) sim = 32;
        int64_t *keep_keys = NULL;
        size_t keep_len = 0;
        size_t keep_cap = 0;
        mc_conn_snapshot_t conn_snapshot = {0};

        if (server_snapshot_conns(s, &conn_snapshot) != 0) {
            log_error("tick: failed to snapshot connections");
            do_evict = false;
        }

        for (size_t ci = 0; ci < conn_snapshot.len; ci++) {
            mc_conn_t *c = conn_snapshot.items[ci];
            mc_proto_state_t state = (mc_proto_state_t)atomic_load(&c->state);
            bool closing = atomic_load(&c->closing);
            conn_count++;
            if (state == MC_STATE_PLAY && c->play_ready && !closing) play_ready_count++;
            if (do_evict && state == MC_STATE_PLAY && c->has_pos && !closing) {
                int32_t bx = floor_i32_from_f64(c->x);
                int32_t bz = floor_i32_from_f64(c->z);
                int32_t pcx = block_to_chunk(bx);
                int32_t pcz = block_to_chunk(bz);
                for (int dz = -sim; dz <= sim; dz++) {
                    for (int dx = -sim; dx <= sim; dx++) {
                        int32_t cx = pcx + dx;
                        int32_t cz = pcz + dz;
                        int64_t key = ((int64_t)cx << 32) | (uint32_t)cz;
                        if (keep_keys_push(&keep_keys, &keep_len, &keep_cap, key) != 0) {
                            do_evict = false;
                            break;
                        }
                    }
                    if (!do_evict) break;
                }
            }

            if (updates && updates_len > 0 && state == MC_STATE_PLAY && c->play_ready && !closing) {
                span_start_us = perf ? mc_now_us() : 0;
                for (size_t i = 0; i < updates_len; i++) {
                    const mc_block_update_t *u = &updates[i];
                    block_update_sends++;
                    if (send_block_update(c, u->x, u->y, u->z, u->state_id) != 0) {
                        conn_close(c);
                        break;
                    }
                }
                if (perf) update_broadcast_us += mc_now_us() - span_start_us;
            }
            if (state == MC_STATE_PLAY && !closing) {
                proto_tick_count++;
                span_start_us = perf ? mc_now_us() : 0;
                if (proto_play_tick(c, now) != 0) {
                    conn_close(c);
                }
                if (perf) proto_tick_us += mc_now_us() - span_start_us;
            }
        }

        pthread_mutex_lock(&s->conns_lock);
        span_start_us = perf ? mc_now_us() : 0;
        item_entities_tick(s, now);
        if (perf) item_entities_us = mc_now_us() - span_start_us;
        pthread_mutex_unlock(&s->conns_lock);

        span_start_us = perf ? mc_now_us() : 0;
        for (size_t vi = 0; vi < conn_snapshot.len; vi++) {
            mc_conn_t *viewer = conn_snapshot.items[vi];
            if (atomic_load(&viewer->closing) || atomic_load(&viewer->state) != MC_STATE_PLAY || !viewer->play_ready) continue;
            for (size_t si = 0; si < conn_snapshot.len; si++) {
                mc_conn_t *subject = conn_snapshot.items[si];
                if (subject == viewer || atomic_load(&subject->closing) || atomic_load(&subject->state) != MC_STATE_PLAY ||
                    !subject->play_ready || !subject->has_pos) {
                    continue;
                }
                remote_sync_attempts++;
                if (proto_play_sync_remote_player(viewer, subject) != 0) {
                    conn_close(viewer);
                    break;
                }
            }
        }
        if (perf) remote_sync_us = mc_now_us() - span_start_us;

        pthread_mutex_lock(&s->conns_lock);
        span_start_us = perf ? mc_now_us() : 0;
        for (mc_conn_t *c = s->conns; c; c = c->next) {
            write_flush_count++;
            if (handle_client_write(c) != 0) {
                conn_close(c);
            }
        }
        if (perf) write_flush_us = mc_now_us() - span_start_us;
        pthread_mutex_unlock(&s->conns_lock);
        conn_snapshot_clear(&conn_snapshot);

        if (do_evict && s->world) {
            span_start_us = perf ? mc_now_us() : 0;
            evicted_count = mc_world_evict_outside(s->world, keep_keys, keep_len, WORLD_EVICT_BUDGET);
            if (perf) evict_us = mc_now_us() - span_start_us;
        }
        free(keep_keys);

        if (s->world && updates_len > 0) {
            span_start_us = perf ? mc_now_us() : 0;
            mc_world_clear_updates(s->world);
            if (perf) clear_updates_us = mc_now_us() - span_start_us;
        }

        int64_t tick_end_us = mc_now_us();
        int64_t candidate_next_tick_us = next_tick_us + tick_us;
        if (tick_end_us > candidate_next_tick_us + tick_us) {
            schedule_resync = true;
        }

        if (perf) {
            int64_t total_us = tick_end_us - tick_start_us;
            int64_t slow_us = mc_perf_slow_us();
            int64_t task_wait_avg_us = task_count > 0 ? task_wait_sum_us / (int64_t)task_count : 0;
            bool slow = total_us >= slow_us || task_us >= slow_us || world_tick_us >= slow_us || furnace_tick_us >= slow_us ||
                        update_broadcast_us >= slow_us || proto_tick_us >= slow_us || item_entities_us >= slow_us ||
                        remote_sync_us >= slow_us || write_flush_us >= slow_us || evict_us >= slow_us || clear_updates_us >= slow_us ||
                        tick_late_us >= slow_us || task_wait_max_us >= slow_us;
            if (slow) {
                log_info("perf tick: idx=%llu total=%.3fms late=%.3fms sleep=%.3fms tasks=%.3fms task_wait_avg=%.3fms task_wait_max=%.3fms world=%.3fms furnaces=%.3fms updates=%.3fms proto=%.3fms items=%.3fms remote=%.3fms write=%.3fms evict=%.3fms clear=%.3fms task_count=%zu conns=%zu ready=%zu updates_len=%zu update_sends=%zu proto_ticks=%zu remote_syncs=%zu writes=%zu item_entities=%zu keep=%zu evicted=%zu world_done=%zu world_integrated=%zu world_dirty=%zu world_saves=%zu/%zu/%zu world_jobs=%zu furnaces_scanned=%zu furnaces_ticked=%zu furnaces_changed=%zu furnaces_errors=%zu",
                         (unsigned long long)tick_index,
                         perf_ms(total_us),
                         perf_ms(tick_late_us),
                         perf_ms(sleep_planned_us),
                         perf_ms(task_us),
                         perf_ms(task_wait_avg_us),
                         perf_ms(task_wait_max_us),
                         perf_ms(world_tick_us),
                         perf_ms(furnace_tick_us),
                         perf_ms(update_broadcast_us),
                         perf_ms(proto_tick_us),
                         perf_ms(item_entities_us),
                         perf_ms(remote_sync_us),
                         perf_ms(write_flush_us),
                         perf_ms(evict_us),
                         perf_ms(clear_updates_us),
                         task_count,
                         conn_count,
                         play_ready_count,
                         updates_len,
                         block_update_sends,
                         proto_tick_count,
                         remote_sync_attempts,
                         write_flush_count,
                         s->item_entities_len,
                         keep_len,
                         evicted_count,
                         world_stats.done_seen,
                         world_stats.done_integrated,
                         world_stats.dirty_chunks,
                         world_stats.saves_scanned,
                         world_stats.saves_attempted,
                         world_stats.saves_succeeded,
                         world_stats.jobs_pending,
                         furnace_stats.scanned_entities,
                         furnace_stats.ticked,
                         furnace_stats.changed,
                         furnace_stats.errors);
            }

            uint64_t summary_ticks = mc_perf_summary_ticks();
            if (summary_ticks > 0) {
                perf_accum_add(&summary_total, total_us);
                perf_accum_add(&summary_late, tick_late_us);
                perf_accum_add(&summary_task, task_us);
                perf_accum_add(&summary_world, world_tick_us);
                perf_accum_add(&summary_furnace, furnace_tick_us);
                perf_accum_add(&summary_updates, update_broadcast_us);
                perf_accum_add(&summary_proto, proto_tick_us);
                perf_accum_add(&summary_items, item_entities_us);
                perf_accum_add(&summary_remote, remote_sync_us);
                perf_accum_add(&summary_write, write_flush_us);
                perf_accum_add(&summary_evict, evict_us);
                summary_task_count += task_count;
                summary_update_sends += block_update_sends;
                summary_proto_ticks += proto_tick_count;
                summary_remote_syncs += remote_sync_attempts;
                if (schedule_resync) summary_resyncs++;
                if (summary_total.count >= summary_ticks) {
                    log_info("perf summary: ticks=%llu avg_total=%.3fms max_total=%.3fms avg_late=%.3fms max_late=%.3fms avg_tasks=%.3fms avg_world=%.3fms avg_furnaces=%.3fms avg_updates=%.3fms avg_proto=%.3fms avg_items=%.3fms avg_remote=%.3fms avg_write=%.3fms avg_evict=%.3fms tasks=%llu update_sends=%llu proto_ticks=%llu remote_syncs=%llu resyncs=%llu",
                             (unsigned long long)summary_total.count,
                             perf_accum_avg_ms(&summary_total),
                             perf_ms(summary_total.max_us),
                             perf_accum_avg_ms(&summary_late),
                             perf_ms(summary_late.max_us),
                             perf_accum_avg_ms(&summary_task),
                             perf_accum_avg_ms(&summary_world),
                             perf_accum_avg_ms(&summary_furnace),
                             perf_accum_avg_ms(&summary_updates),
                             perf_accum_avg_ms(&summary_proto),
                             perf_accum_avg_ms(&summary_items),
                             perf_accum_avg_ms(&summary_remote),
                             perf_accum_avg_ms(&summary_write),
                             perf_accum_avg_ms(&summary_evict),
                             (unsigned long long)summary_task_count,
                             (unsigned long long)summary_update_sends,
                             (unsigned long long)summary_proto_ticks,
                             (unsigned long long)summary_remote_syncs,
                             (unsigned long long)summary_resyncs);
                    summary_total = (perf_accum_t){0};
                    summary_late = (perf_accum_t){0};
                    summary_task = (perf_accum_t){0};
                    summary_world = (perf_accum_t){0};
                    summary_furnace = (perf_accum_t){0};
                    summary_updates = (perf_accum_t){0};
                    summary_proto = (perf_accum_t){0};
                    summary_items = (perf_accum_t){0};
                    summary_remote = (perf_accum_t){0};
                    summary_write = (perf_accum_t){0};
                    summary_evict = (perf_accum_t){0};
                    summary_task_count = 0;
                    summary_update_sends = 0;
                    summary_proto_ticks = 0;
                    summary_remote_syncs = 0;
                    summary_resyncs = 0;
                }
            }
        }

        next_tick_us = candidate_next_tick_us;
        if (schedule_resync) {
            next_tick_us = tick_end_us + tick_us;
        }

        tick_index++;
    }

    mc_task_t *task = mc_task_queue_drain(&s->task_queue);
    while (task) {
        mc_task_t *next = task->next;
        if (task->payload) free(task->payload);
        if (task->conn) conn_release(task->conn);
        free(task);
        task = next;
    }
    return NULL;
}

static void server_cleanup(mc_server_t *s) {
    mc_conn_t **to_close = NULL;
    size_t to_close_len = 0;
    pthread_mutex_lock(&s->conns_lock);
    for (mc_conn_t *c = s->conns; c; c = c->next) {
        if (!c->closing) continue;
        pthread_mutex_lock(&c->out_lock);
        bool can_close = (c->out.len == 0);
        pthread_mutex_unlock(&c->out_lock);
        if (can_close) {
            mc_conn_t **next = (mc_conn_t **)realloc(to_close, (to_close_len + 1) * sizeof(*to_close));
            if (next) {
                to_close = next;
                to_close[to_close_len++] = c;
            }
        }
    }
    pthread_mutex_unlock(&s->conns_lock);

    for (size_t i = 0; i < to_close_len; i++) {
        server_close_conn(s, to_close[i]);
    }
    free(to_close);
}

int net_server_run(mc_server_t *s) {
    if (!s) return -1;
    atomic_store(&s->running, true);
    if (pthread_create(&s->tick_thread, NULL, tick_thread_main, s) != 0) {
        atomic_store(&s->running, false);
        return -1;
    }
    struct epoll_event events[MAX_EVENTS];

    while (atomic_load(&s->running)) {
        int n = epoll_wait(s->epoll_fd, events, MAX_EVENTS, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            atomic_store(&s->running, false);
            break;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.ptr == s) {
                for (;;) {
                    struct sockaddr_in caddr;
                    socklen_t clen = sizeof(caddr);
                    int cfd = accept(s->listen_fd, (struct sockaddr *)&caddr, &clen);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        break;
                    }
                    if (set_nonblocking(cfd) != 0) {
                        close(cfd);
                        continue;
                    }
                    mc_conn_t *c = conn_create(s, cfd);
                    if (!c) {
                        close(cfd);
                        continue;
                    }
                    add_epoll(s->epoll_fd, cfd, EPOLLIN | EPOLLET, c);
                }
            } else {
                mc_conn_t *c = (mc_conn_t *)events[i].data.ptr;
                if (events[i].events & EPOLLIN) {
                    if (handle_client_read(s, c) != 0) {
                        server_close_conn(s, c);
                        continue;
                    }
                }
            }
        }

        server_cleanup(s);
    }

    atomic_store(&s->running, false);
    pthread_join(s->tick_thread, NULL);
    return 0;
}

void net_server_stop(mc_server_t *s) {
    if (!s) return;
    atomic_store(&s->running, false);
}

void net_server_destroy(mc_server_t *s) {
    if (!s) return;
    pthread_mutex_lock(&s->conns_lock);
    mc_conn_t *c = s->conns;
    s->conns = NULL;
    pthread_mutex_unlock(&s->conns_lock);
    while (c) {
        mc_conn_t *next = c->next;
        if (atomic_load(&c->state) == MC_STATE_PLAY || c->player) {
            proto_play_conn_cleanup(c);
        }
        conn_destroy(c);
        c = next;
    }
    close(s->epoll_fd);
    close(s->listen_fd);
    for (size_t i = 0; i < s->item_entities_len; i++) {
        mc_slot_clear(&s->item_entities[i].slot);
    }
    free(s->item_entities);
    mc_world_destroy(s->world);
    mc_task_queue_destroy(&s->task_queue);
    pthread_mutex_destroy(&s->conns_lock);
    free(s);
}

mc_conn_t *net_server_find_conn_by_name(mc_server_t *server, const char *name) {
    if (!server || !name || !*name) return NULL;
    pthread_mutex_lock(&server->conns_lock);
    mc_conn_t *c = server->conns;
    while (c) {
        if (strcmp(c->username, name) == 0) {
            conn_acquire(c);
            pthread_mutex_unlock(&server->conns_lock);
            return c;
        }
        c = c->next;
    }
    pthread_mutex_unlock(&server->conns_lock);
    return NULL;
}

void net_server_release_conn(mc_conn_t *conn) {
    conn_release(conn);
}

mc_world_t *net_server_world(mc_server_t *server) {
    if (!server) return NULL;
    return server->world;
}

mc_difficulty_t net_server_get_difficulty(mc_server_t *server) {
    if (!server) return MC_DIFFICULTY_NORMAL;
    return normalize_difficulty((mc_difficulty_t)atomic_load(&server->difficulty));
}

void net_server_set_difficulty(mc_server_t *server, mc_difficulty_t difficulty) {
    if (!server) return;
    difficulty = normalize_difficulty(difficulty);
    atomic_store(&server->difficulty, (int)difficulty);
    server->cfg.difficulty = difficulty;
}

const char *mc_difficulty_name(mc_difficulty_t difficulty) {
    switch (normalize_difficulty(difficulty)) {
        case MC_DIFFICULTY_PEACEFUL: return "peaceful";
        case MC_DIFFICULTY_EASY: return "easy";
        case MC_DIFFICULTY_NORMAL: return "normal";
        case MC_DIFFICULTY_HARD: return "hard";
        default: return "normal";
    }
}

int net_server_broadcast_difficulty(mc_server_t *server) {
    if (!server) return -1;

    int rc = 0;
    mc_difficulty_t difficulty = net_server_get_difficulty(server);
    pthread_mutex_lock(&server->conns_lock);
    for (mc_conn_t *c = server->conns; c; c = c->next) {
        c->next_natural_regen_ms = 0;
        c->next_starvation_damage_ms = 0;
        if (difficulty == MC_DIFFICULTY_PEACEFUL) {
            c->food_exhaustion = 0.0f;
            if (c->player) c->player->food_exhaustion = 0.0f;
        }
        if (c->closing || c->state != MC_STATE_PLAY || !c->play_init_sent) continue;
        if (proto_play_send_difficulty(c) != 0) {
            conn_close(c);
            rc = -1;
        }
    }
    pthread_mutex_unlock(&server->conns_lock);
    return rc;
}

int net_server_spawn_item_drop(mc_server_t *server, double x, double y, double z, const mc_slot_t *slot) {
    return net_server_spawn_item_drop_with_motion(server, x, y, z, 0.0, 0.0, 0.0, slot, ITEM_ENTITY_DEFAULT_PICKUP_DELAY_TICKS);
}

int net_server_spawn_item_drop_locked(mc_server_t *server, double x, double y, double z, const mc_slot_t *slot) {
    return net_server_spawn_item_drop_locked_with_pickup_delay(server, x, y, z, slot, ITEM_ENTITY_DEFAULT_PICKUP_DELAY_TICKS);
}

int net_server_spawn_item_drop_with_pickup_delay(mc_server_t *server, double x, double y, double z, const mc_slot_t *slot,
                                                 int32_t pickup_delay_ticks) {
    return net_server_spawn_item_drop_with_motion(server, x, y, z, 0.0, 0.0, 0.0, slot, pickup_delay_ticks);
}

int net_server_spawn_item_drop_with_motion(mc_server_t *server, double x, double y, double z, double vx, double vy, double vz,
                                           const mc_slot_t *slot, int32_t pickup_delay_ticks) {
    if (!server || !slot || !slot->present || slot->count <= 0) return -1;
    pthread_mutex_lock(&server->conns_lock);
    int rc = net_server_spawn_item_drop_locked_with_pickup_delay(server, x, y, z, slot, pickup_delay_ticks);
    if (rc == 0 && server->item_entities_len > 0) {
        mc_item_entity_t *item = &server->item_entities[server->item_entities_len - 1];
        item->vx = vx;
        item->vy = vy;
        item->vz = vz;
    }
    pthread_mutex_unlock(&server->conns_lock);
    return rc;
}

int net_server_spawn_item_drop_locked_with_pickup_delay(mc_server_t *server, double x, double y, double z, const mc_slot_t *slot,
                                                        int32_t pickup_delay_ticks) {
    if (!server || !slot || !slot->present || slot->count <= 0) return -1;
    if (server->item_entities_len == server->item_entities_cap) {
        size_t new_cap = server->item_entities_cap ? server->item_entities_cap * 2 : 16;
        mc_item_entity_t *next = (mc_item_entity_t *)realloc(server->item_entities, new_cap * sizeof(*next));
        if (!next) {
            return -1;
        }
        server->item_entities = next;
        server->item_entities_cap = new_cap;
    }

    mc_item_entity_t entity;
    memset(&entity, 0, sizeof(entity));
    entity.entity_id = server->next_entity_id++;
    fill_entity_uuid_from_id(entity.entity_id, entity.uuid);
    entity.x = x;
    entity.y = y;
    entity.z = z;
    entity.vx = 0.0;
    entity.vy = 0.0;
    entity.vz = 0.0;
    entity.expires_at_ms = now_ms() + ITEM_ENTITY_TTL_MS;
    entity.pickup_delay_ticks = pickup_delay_ticks > 0 ? pickup_delay_ticks : 0;
    if (mc_slot_copy(&entity.slot, slot) != 0) {
        return -1;
    }

    server->item_entities[server->item_entities_len++] = entity;
    mc_item_entity_t *stored = &server->item_entities[server->item_entities_len - 1];
    for (mc_conn_t *c = server->conns; c; c = c->next) {
        if (c->closing || c->state != MC_STATE_PLAY || !c->play_ready) continue;
        if (send_item_entity_spawn(c, stored) != 0 || send_item_entity_metadata(c, stored) != 0) {
            conn_close(c);
        }
    }
    return 0;
}

int net_server_sync_item_entities_to_conn(mc_server_t *server, mc_conn_t *conn) {
    if (!server || !conn) return -1;
    pthread_mutex_lock(&server->conns_lock);
    if (conn->closing || conn->state != MC_STATE_PLAY || !conn->play_ready) {
        pthread_mutex_unlock(&server->conns_lock);
        return 0;
    }
    for (size_t i = 0; i < server->item_entities_len; i++) {
        mc_item_entity_t *item = &server->item_entities[i];
        if (send_item_entity_spawn(conn, item) != 0 || send_item_entity_metadata(conn, item) != 0) {
            conn_close(conn);
            pthread_mutex_unlock(&server->conns_lock);
            return -1;
        }
    }
    pthread_mutex_unlock(&server->conns_lock);
    return 0;
}

int net_server_resolve_item_entities_for_block(mc_server_t *server, int32_t x, int32_t y, int32_t z, int32_t state_id) {
    if (!server || !server->world) return 0;
    const mc_world_ids_t *ids = mc_world_ids(server->world);
    if (!ids || block_state_is_passable(ids, state_id)) return 0;

    int rc = 0;
    pthread_mutex_lock(&server->conns_lock);
    for (size_t i = 0; i < server->item_entities_len; i++) {
        mc_item_entity_t *item = &server->item_entities[i];
        if (!item_entity_intersects_block_at(item, x, y, z)) continue;
        if (!relocate_item_entity_out_of_block(server, item, x, y, z)) continue;
        if (broadcast_item_entity_teleport(server, item) != 0) rc = -1;
    }
    pthread_mutex_unlock(&server->conns_lock);
    return rc;
}

void net_server_close_container_viewers(mc_server_t *server, mc_container_kind_t kind, int32_t x, int32_t y, int32_t z) {
    if (!server || kind == MC_CONTAINER_KIND_NONE) return;
    pthread_mutex_lock(&server->conns_lock);
    for (mc_conn_t *c = server->conns; c; c = c->next) {
        if (c->closing || !c->active_window.open || !c->active_window.container) continue;
        mc_container_instance_t *container = c->active_window.container;
        if (container->kind != kind || container->x != x || container->y != y || container->z != z) continue;
        close_matching_container_window(c);
    }
    pthread_mutex_unlock(&server->conns_lock);
}

int net_server_get_open_container_snapshot(mc_server_t *server, mc_container_kind_t kind, int32_t x, int32_t y, int32_t z,
                                           mc_container_instance_t *out) {
    if (!server || !out || kind == MC_CONTAINER_KIND_NONE) return -1;
    pthread_mutex_lock(&server->conns_lock);
    for (mc_conn_t *c = server->conns; c; c = c->next) {
        if (c->closing || !c->active_window.open || !c->active_window.container) continue;
        mc_container_instance_t *container = c->active_window.container;
        if (container->kind != kind || container->x != x || container->y != y || container->z != z) continue;
        mc_container_instance_init(out, container->kind, container->x, container->y, container->z);
        out->slot_count = container->slot_count;
        out->state_id = container->state_id;
        out->dirty = container->dirty;
        out->furnace_burn_time = container->furnace_burn_time;
        out->furnace_burn_duration = container->furnace_burn_duration;
        out->furnace_cook_time = container->furnace_cook_time;
        out->furnace_cook_duration = container->furnace_cook_duration;
        for (int i = 0; i < MC_CONTAINER_SLOT_COUNT; i++) {
            if (mc_slot_copy(&out->slots[i], &container->slots[i]) != 0) {
                mc_container_instance_clear(out);
                pthread_mutex_unlock(&server->conns_lock);
                return -1;
            }
        }
        pthread_mutex_unlock(&server->conns_lock);
        return 0;
    }
    pthread_mutex_unlock(&server->conns_lock);
    return 1;
}
