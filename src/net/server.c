/*
 * Network accept loop and main server tick. This file is the bridge between
 * epoll-driven socket IO and the gameplay/world modules:
 * - the network side decodes packets and pushes tasks
 * - the tick thread drains tasks and mutates world state; only the network
 *   owner flushes outbound bytes and changes epoll registrations
 *
 * When reading this file, keep the lock model in mind: long gameplay work
 * should happen without holding conns_lock, using refcounted snapshots when
 * iterating over active connections.
 */
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
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>

#define MAX_EVENTS 64
#define MAX_WRITE_BYTES_PER_PASS (256u * 1024u)
#define PERF_TICK_BIN_US 100u
#define PERF_TICK_BIN_MAX 10000u
#define PKT_PLAY_BLOCK_UPDATE MC_PKT_PLAY_CLIENTBOUND_BLOCK_UPDATE
#define PKT_PLAY_SPAWN_ENTITY MC_PKT_PLAY_CLIENTBOUND_ADD_ENTITY
#define PKT_PLAY_ENTITY_METADATA MC_PKT_PLAY_CLIENTBOUND_SET_ENTITY_DATA
#define PKT_PLAY_ENTITY_DESTROY MC_PKT_PLAY_CLIENTBOUND_REMOVE_ENTITIES
#define PKT_PLAY_ENTITY_TELEPORT MC_PKT_PLAY_CLIENTBOUND_TELEPORT_ENTITY
#define PKT_PLAY_ENTITY_VELOCITY MC_PKT_PLAY_CLIENTBOUND_SET_ENTITY_MOTION
#define PKT_PLAY_TAKE_ITEM_ENTITY MC_PKT_PLAY_CLIENTBOUND_TAKE_ITEM_ENTITY
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

typedef struct {
    uint64_t count;
    int64_t total_us;
    int64_t max_us;
} mc_packet_perf_t;
/* Runtime entity used for block/item drops. It is owned by the server tick,
 * not by the world module, because pickup and networking are connection-aware. */

typedef struct mc_network_worker {
    struct mc_server *server;
    unsigned index;
    int epoll_fd;
    int wake_fd;
    pthread_t thread;
    pthread_mutex_t command_lock;
    mc_conn_t *command_head;
    mc_conn_t *command_tail;
    size_t command_len;
    mc_conn_t *owned; /* only this worker changes the list */
    mc_conn_t *retired; /* retain epoll-batch pointers until the batch ends */
    atomic_size_t connections;
    atomic_size_t connections_peak;
    atomic_size_t commands_peak;
    atomic_uint_fast64_t accepted;
    atomic_uint_fast64_t packets;
    atomic_uint_fast64_t bytes_in;
    atomic_uint_fast64_t bytes_out;
    atomic_uint_fast64_t epoll_wait_us;
    atomic_uint_fast64_t epoll_calls;
    atomic_uint_fast64_t task_wait_us;
    atomic_uint_fast64_t task_wait_count;
    atomic_uint_fast64_t task_wait_max_us;
    atomic_uint_fast64_t disconnected;
    int64_t started_us;
} mc_network_worker_t;

/* The caller owns the acceptor epoll; workers each own a separate epoll and
 * all socket operations for their stable subset of connections. */
struct mc_server {
    int listen_fd;
    int epoll_fd;
    int wake_fd;
    int signal_fd;
    mc_server_config_t cfg;
    atomic_bool running;
    mc_conn_t *conns;
    mc_conn_t *departures; /* conns_lock; tick owns gameplay departure handling */
    size_t active_connections; /* guarded by conns_lock */
    atomic_int next_entity_id;
    atomic_bool stop_requested;
    atomic_bool network_error;
    int network_worker_count;
    mc_network_worker_t workers[4];
    unsigned next_worker; /* acceptor-owned round-robin cursor */
    pthread_mutex_t conns_lock;
    mc_task_queue_t task_queue;
    pthread_t tick_thread;
    atomic_uint_fast64_t network_write_us;
    atomic_uint_fast64_t network_flush_count;
    int64_t task_handler_us; /* tick-thread only, includes fast packet passes */
    int64_t task_handler_max_us;
    size_t task_handler_count;
    int64_t fast_pass_us;
    size_t fast_pass_tasks;
    mc_packet_perf_t play_packet_perf[128]; /* tick-thread only */
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

static bool debug_env_enabled(const char *name) {
    const char *env = name ? getenv(name) : NULL;
    return env && *env && strcmp(env, "0") != 0;
}

static bool debug_items_enabled(void) {
    return debug_env_enabled("MC_DEBUG_ITEMS");
}

static bool debug_pickup_enabled(void) {
    return debug_env_enabled("MC_DEBUG_PICKUP");
}

static bool debug_net_latency_enabled(void) {
    return debug_env_enabled("MC_DEBUG_NET_LATENCY");
}

static bool debug_broadcast_enabled(void) {
    return debug_env_enabled("MC_DEBUG_BROADCAST");
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
        if (id == MC_PKT_PLAY_SERVERBOUND_MOVE_PLAYER_STATUS_ONLY) return false;
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

static void wake_eventfd(int wake_fd) {
    if (wake_fd < 0) return;
    uint64_t one = 1;
    ssize_t n;
    do {
        n = write(wake_fd, &one, sizeof(one));
    } while (n < 0 && errno == EINTR);
    /* EAGAIN means the event counter is already readable. */
}

static void conn_notify_output(mc_conn_t *c) {
    if (c && c->server && c->network_worker_index < (unsigned)c->server->network_worker_count)
        wake_eventfd(c->server->workers[c->network_worker_index].wake_fd);
}

static void conn_destroy(mc_conn_t *c);

static mc_conn_t *conn_create(mc_server_t *s, int fd, unsigned worker_index) {
    mc_conn_t *c = (mc_conn_t *)calloc(1, sizeof(mc_conn_t));
    if (!c) return NULL;
    atomic_init(&c->fd, fd);
    c->state = MC_STATE_HANDSHAKING;
    c->cfg = &s->cfg;
    c->server = s;
    c->network_worker_index = worker_index;
    c->notify_output = conn_notify_output;
    c->command_authorized = s->cfg.admin_commands;
    c->entity_id = atomic_fetch_add(&s->next_entity_id, 1);
    c->block_ack_sequence = -1;
    atomic_init(&c->refcount, 1);
    atomic_init(&c->detached, false);
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
    if (s->active_connections >= (size_t)s->cfg.max_connections) {
        pthread_mutex_unlock(&s->conns_lock);
        c->fd = -1; /* the acceptor retains ownership of the rejected fd */
        conn_destroy(c);
        return NULL;
    }
    c->in.limit = MC_MAX_INPUT_BUFFER;
    c->out.limit = MC_MAX_OUTPUT_BUFFER;
    s->active_connections++;
    c->next = s->conns;
    s->conns = c;
    pthread_mutex_unlock(&s->conns_lock);
    return c;
}

static void conn_destroy(mc_conn_t *c) {
    if (!c) return;
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
            s->active_connections--;
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
    /* Snapshot with refcounts so the tick thread can spend time on chunk
     * streaming and protocol work without pinning conns_lock the whole time. */
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
    mc_network_worker_t *worker = &s->workers[c->network_worker_index];
    if (atomic_exchange(&c->detached, true)) return;
    atomic_store(&c->closing, true);
    pthread_mutex_lock(&c->out_lock);
    c->abort_io = true;
    if (c->owner_registered) del_epoll(worker->epoll_fd, c->fd);
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    pthread_mutex_unlock(&c->out_lock);
    if (c->owner_registered) {
        mc_conn_t **pp = &worker->owned;
        while (*pp && *pp != c) pp = &(*pp)->owner_next;
        if (*pp == c) {
            *pp = c->owner_next;
            atomic_fetch_sub(&worker->connections, 1);
        }
        c->owner_next = NULL;
        c->owner_registered = false;
    }
    conn_remove(s, c);
    atomic_fetch_add(&worker->disconnected, 1);

    /* The tick thread alone updates other players' view of this player. The
     * extra ref survives even if the network batch has already finished. */
    conn_acquire(c);
    pthread_mutex_lock(&s->conns_lock);
    c->departure_next = s->departures;
    s->departures = c;
    pthread_mutex_unlock(&s->conns_lock);
    c->retired_next = worker->retired;
    worker->retired = c;
}

static void server_release_retired(mc_network_worker_t *worker) {
    mc_conn_t *c = worker->retired;
    worker->retired = NULL;
    while (c) {
        mc_conn_t *next = c->retired_next;
        conn_release(c);
        c = next;
    }
}

static void server_process_departures(mc_server_t *s) {
    pthread_mutex_lock(&s->conns_lock);
    mc_conn_t *departed = s->departures;
    s->departures = NULL;
    pthread_mutex_unlock(&s->conns_lock);
    if (!departed) return;

    mc_conn_snapshot_t viewers = {0};
    bool have_viewers = server_snapshot_conns(s, &viewers) == 0;
    if (!have_viewers) log_error("departure: failed to snapshot connections");
    while (departed) {
        mc_conn_t *next = departed->departure_next;
        departed->departure_next = NULL;
        if (have_viewers && atomic_load(&departed->state) == MC_STATE_PLAY) {
            for (size_t i = 0; i < viewers.len; i++) {
                mc_conn_t *viewer = viewers.items[i];
                if (atomic_load(&viewer->closing) || atomic_load(&viewer->state) != MC_STATE_PLAY) continue;
                if (proto_play_remove_remote_player(viewer, departed) != 0) conn_close(viewer);
            }
        }
        proto_play_conn_cleanup(departed);
        conn_release(departed);
        departed = next;
    }
    conn_snapshot_clear(&viewers);
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

static int64_t tick_thread_cpu_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* Nearest-rank percentile, quantized to 0.1 ms. The last bin represents
 * durations >= 1000 ms; exact maxima remain in perf_accum_t. */
static double perf_tick_percentile_ms(const uint32_t *bins, uint64_t count, unsigned percentile) {
    if (!bins || count == 0 || percentile == 0 || percentile > 100) return 0.0;
    uint64_t target = (count * percentile + 99u) / 100u;
    uint64_t seen = 0;
    for (size_t i = 0; i <= PERF_TICK_BIN_MAX; i++) {
        seen += bins[i];
        if (seen >= target) return (double)i * ((double)PERF_TICK_BIN_US / 1000.0);
    }
    return (double)PERF_TICK_BIN_MAX * ((double)PERF_TICK_BIN_US / 1000.0);
}

static void network_worker_record_task_wait(mc_server_t *s, const mc_task_t *task, int64_t now_ms_value) {
    if (!task || !task->conn || task->conn->network_worker_index >= (unsigned)s->network_worker_count) return;
    mc_network_worker_t *worker = &s->workers[task->conn->network_worker_index];
    uint64_t wait_us = now_ms_value > task->enqueue_ms ? (uint64_t)(now_ms_value - task->enqueue_ms) * 1000u : 0;
    atomic_fetch_add(&worker->task_wait_us, wait_us);
    atomic_fetch_add(&worker->task_wait_count, 1);
    uint64_t old = atomic_load(&worker->task_wait_max_us);
    while (wait_us > old && !atomic_compare_exchange_weak(&worker->task_wait_max_us, &old, wait_us)) {}
}

static void log_network_workers_stats(mc_server_t *s) {
    int64_t now_us = mc_now_us();
    pthread_mutex_lock(&s->task_queue.lock);
    size_t task_depth = s->task_queue.len;
    pthread_mutex_unlock(&s->task_queue.lock);
    for (int i = 0; i < s->network_worker_count; i++) {
        mc_network_worker_t *worker = &s->workers[i];
        pthread_mutex_lock(&worker->command_lock);
        size_t command_depth = worker->command_len;
        pthread_mutex_unlock(&worker->command_lock);
        clockid_t cpu_clock;
        struct timespec cpu_time = {0};
        double cpu_pct = 0.0;
        if (pthread_getcpuclockid(worker->thread, &cpu_clock) == 0 &&
            clock_gettime(cpu_clock, &cpu_time) == 0 && now_us > worker->started_us) {
            uint64_t cpu_us = (uint64_t)cpu_time.tv_sec * 1000000u + (uint64_t)cpu_time.tv_nsec / 1000u;
            cpu_pct = (double)cpu_us * 100.0 / (double)(now_us - worker->started_us);
        }
        uint64_t wait_count = atomic_load(&worker->task_wait_count);
        log_info("perf network: worker=%d connections=%zu peak=%zu accepted=%llu disconnected=%llu packets=%llu bytes_in=%llu bytes_out=%llu command_depth=%zu command_peak=%zu task_depth=%zu task_wait_avg=%.3fms task_wait_max=%.3fms epoll_calls=%llu epoll_wait=%.3fms cpu=%.1f%%",
                 i, atomic_load(&worker->connections), atomic_load(&worker->connections_peak),
                 (unsigned long long)atomic_load(&worker->accepted),
                 (unsigned long long)atomic_load(&worker->disconnected),
                 (unsigned long long)atomic_load(&worker->packets),
                 (unsigned long long)atomic_load(&worker->bytes_in),
                 (unsigned long long)atomic_load(&worker->bytes_out),
                 command_depth, atomic_load(&worker->commands_peak), task_depth,
                 wait_count ? (double)atomic_load(&worker->task_wait_us) / (double)wait_count / 1000.0 : 0.0,
                 (double)atomic_load(&worker->task_wait_max_us) / 1000.0,
                 (unsigned long long)atomic_load(&worker->epoll_calls),
                 (double)atomic_load(&worker->epoll_wait_us) / 1000.0, cpu_pct);
    }
}

static void network_workers_destroy(mc_server_t *s) {
    for (int i = 0; i < s->network_worker_count; i++) {
        mc_network_worker_t *worker = &s->workers[i];
        close(worker->wake_fd);
        close(worker->epoll_fd);
        pthread_mutex_destroy(&worker->command_lock);
    }
    s->network_worker_count = 0;
}

static int network_workers_init(mc_server_t *s, int count) {
    for (int i = 0; i < count; i++) {
        mc_network_worker_t *worker = &s->workers[i];
        worker->server = s;
        worker->index = (unsigned)i;
        worker->epoll_fd = -1;
        worker->wake_fd = -1;
        if (pthread_mutex_init(&worker->command_lock, NULL) != 0) goto fail;
        worker->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
        if (worker->epoll_fd < 0) goto fail_current;
        worker->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (worker->wake_fd < 0 ||
            add_epoll(worker->epoll_fd, worker->wake_fd, EPOLLIN, worker) != 0) goto fail_current;
        s->network_worker_count++;
    }
    return 0;

fail_current:
    if (s->workers[s->network_worker_count].wake_fd >= 0) close(s->workers[s->network_worker_count].wake_fd);
    if (s->workers[s->network_worker_count].epoll_fd >= 0) close(s->workers[s->network_worker_count].epoll_fd);
    pthread_mutex_destroy(&s->workers[s->network_worker_count].command_lock);
fail:
    network_workers_destroy(s);
    return -1;
}

int net_server_init(mc_server_t **out, const mc_server_config_t *cfg) {
    if (!out || !cfg) return -1;

    mc_server_t *s = (mc_server_t *)calloc(1, sizeof(mc_server_t));
    if (!s) return -1;
    s->wake_fd = -1;
    s->signal_fd = -1;
    s->cfg = *cfg;
    if (cfg->max_connections <= 0 || (unsigned)cfg->max_connections > MC_MAX_CONNECTIONS ||
        (cfg->network_workers != 0 && cfg->network_workers != 1 &&
         cfg->network_workers != 2 && cfg->network_workers != 4)) { free(s); return -1; }
    if (s->cfg.network_workers == 0) s->cfg.network_workers = 1;
    s->cfg.difficulty = normalize_difficulty(cfg->difficulty);
    atomic_init(&s->next_entity_id, 1);
    s->conns = NULL;
    atomic_init(&s->running, false);
    atomic_init(&s->stop_requested, false);
    atomic_init(&s->network_error, false);
    atomic_init(&s->difficulty, (int)s->cfg.difficulty);
    atomic_init(&s->network_write_us, 0);
    atomic_init(&s->network_flush_count, 0);
    if (pthread_mutex_init(&s->conns_lock, NULL) != 0) {
        free(s);
        return -1;
    }
    if (mc_task_queue_init(&s->task_queue) != 0) {
        pthread_mutex_destroy(&s->conns_lock);
        free(s);
        return -1;
    }

    s->world = mc_world_create_with_workers(cfg->world_path, cfg->level_seed, cfg->chunk_workers);
    if (!s->world) {
        mc_task_queue_destroy(&s->task_queue);
        pthread_mutex_destroy(&s->conns_lock);
        free(s);
        return -1;
    }
    log_info("runtime threads: acceptor=1 network_owners=%d tick_threads=1 chunk_workers=%d save_workers=0 simulation_workers=0",
             s->cfg.network_workers,
             mc_world_worker_count(s->world));

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

    s->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (s->wake_fd < 0 || add_epoll(s->epoll_fd, s->wake_fd, EPOLLIN, &s->wake_fd) != 0) {
        log_error("failed to initialize network wakeup (errno=%d %s)", errno, strerror(errno));
        if (s->wake_fd >= 0) close(s->wake_fd);
        close(s->epoll_fd);
        close(s->listen_fd);
        mc_world_destroy(s->world);
        mc_task_queue_destroy(&s->task_queue);
        pthread_mutex_destroy(&s->conns_lock);
        free(s);
        return -1;
    }

    if (network_workers_init(s, s->cfg.network_workers) != 0) {
        log_error("failed to initialize network workers (errno=%d %s)", errno, strerror(errno));
        close(s->wake_fd);
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

static bool item_entity_has_motion(const mc_item_entity_t *item) {
    if (!item) return false;
    return fabs(item->vx) > 0.0001 || fabs(item->vy) > 0.0001 || fabs(item->vz) > 0.0001;
}

static int16_t entity_velocity_short(double velocity) {
    double scaled = velocity * 8000.0;
    if (scaled > 32767.0) return 32767;
    if (scaled < -32768.0) return -32768;
    return (int16_t)(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);
}

static int send_item_entity_velocity(mc_conn_t *c, const mc_item_entity_t *item) {
    if (!c || !item) return -1;
    uint8_t buf[32];
    size_t pos = 0;
    if (w_varint(buf, sizeof(buf), &pos, item->entity_id) != 0) return -1;
    if (w_u16_be(buf, sizeof(buf), &pos, (uint16_t)entity_velocity_short(item->vx)) != 0 ||
        w_u16_be(buf, sizeof(buf), &pos, (uint16_t)entity_velocity_short(item->vy)) != 0 ||
        w_u16_be(buf, sizeof(buf), &pos, (uint16_t)entity_velocity_short(item->vz)) != 0) {
        return -1;
    }
    return conn_write_packet(c, PKT_PLAY_ENTITY_VELOCITY, buf, pos, -1);
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

static int send_take_item_entity(mc_conn_t *c, int32_t item_entity_id, int32_t collector_entity_id, int32_t count) {
    if (!c) return -1;
    uint8_t buf[32];
    size_t pos = 0;
    if (count < 1) count = 1;
    if (w_varint(buf, sizeof(buf), &pos, item_entity_id) != 0 ||
        w_varint(buf, sizeof(buf), &pos, collector_entity_id) != 0 ||
        w_varint(buf, sizeof(buf), &pos, count) != 0) {
        return -1;
    }
    return conn_write_packet(c, PKT_PLAY_TAKE_ITEM_ENTITY, buf, pos, -1);
}

static int broadcast_take_item_entity(mc_server_t *s, int32_t item_entity_id, int32_t collector_entity_id, int32_t count) {
    if (!s) return -1;
    int rc = 0;
    for (mc_conn_t *c = s->conns; c; c = c->next) {
        if (c->closing || c->state != MC_STATE_PLAY || !c->play_ready) continue;
        if (send_take_item_entity(c, item_entity_id, collector_entity_id, count) != 0) {
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
    int32_t item_id = s->item_entities[idx].slot.present ? s->item_entities[idx].slot.item_id : -1;
    int32_t count = s->item_entities[idx].slot.present ? s->item_entities[idx].slot.count : 0;
    if (debug_items_enabled()) {
        const char *item_name = item_id >= 0 ? mc_minecraft_item_name(item_id) : NULL;
        log_info("items debug: t=%lld destroy eid=%d item=%s(%d) count=%d",
                 (long long)now_ms(), entity_id, item_name ? item_name : "(empty)", item_id, count);
    }
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
            if (debug_items_enabled()) {
                const char *item_name = item->slot.present ? mc_minecraft_item_name(item->slot.item_id) : NULL;
                log_info("items debug: t=%lld expire eid=%d item=%s(%d) count=%d",
                         (long long)now_ms(), item->entity_id, item_name ? item_name : "(empty)",
                         item->slot.present ? item->slot.item_id : -1,
                         item->slot.present ? item->slot.count : 0);
            }
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
                if (c->closing || c->state != MC_STATE_PLAY || !c->play_ready || !c->has_pos || c->dead) continue;
                double dx = c->x - item->x;
                double dy = c->y - item->y;
                double dz = c->z - item->z;
                double dist_sq = dx * dx + dy * dy + dz * dz;
                if (dist_sq > ITEM_ENTITY_PICKUP_RADIUS_SQ) continue;

                int32_t item_id = item->slot.present ? item->slot.item_id : -1;
                int32_t before_count = item->slot.present ? item->slot.count : 0;
                if (debug_pickup_enabled()) {
                    const char *item_name = item_id >= 0 ? mc_minecraft_item_name(item_id) : NULL;
                    log_info("pickup debug: t=%lld attempt player=%s collector_eid=%d item_eid=%d item=%s(%d) count=%d dist_sq=%.3f delay=%d",
                             (long long)now_ms(), c->username[0] ? c->username : "(unknown)", c->entity_id, item->entity_id,
                             item_name ? item_name : "(empty)", item_id, before_count, dist_sq, item->pickup_delay_ticks);
                }
                int pickup_rc = proto_play_try_pickup_ground_slot(c, &item->slot);
                if (pickup_rc < 0) {
                    conn_close(c);
                    continue;
                }
                if (pickup_rc == 0) continue;
                int32_t after_count = item->slot.present ? item->slot.count : 0;
                int32_t picked_count = before_count - after_count;
                if (picked_count <= 0) picked_count = pickup_rc;
                if (debug_pickup_enabled()) {
                    const char *item_name = item_id >= 0 ? mc_minecraft_item_name(item_id) : NULL;
                    log_info("pickup debug: t=%lld success player=%s collector_eid=%d item_eid=%d item=%s(%d) picked=%d remaining=%d",
                             (long long)now_ms(), c->username[0] ? c->username : "(unknown)", c->entity_id, item->entity_id,
                             item_name ? item_name : "(empty)", item_id, picked_count, after_count);
                }
                if (broadcast_take_item_entity(s, item->entity_id, c->entity_id, picked_count) != 0) {
                    /* Individual failing viewers were closed; keep the authoritative pickup. */
                }
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
    mc_network_worker_t *worker = &s->workers[c->network_worker_index];
    uint8_t buf[4096];
    for (;;) {
        ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
        if (n == 0) return -1;
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        atomic_fetch_add(&worker->bytes_in, (uint64_t)n);
        buf_compact(&c->in);
        c->in.limit = MC_MAX_INPUT_BUFFER;
        if (buf_write(&c->in, buf, (size_t)n) != 0) return -1;

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
        if (debug_net_latency_enabled()) {
            const char *packet_name = serverbound_packet_name(c->state, frame.packet_id);
            pthread_mutex_lock(&c->out_lock);
            size_t out_pending = c->out.len > c->out.rpos ? c->out.len - c->out.rpos : 0;
            pthread_mutex_unlock(&c->out_lock);
            log_info("net latency: t=%lld recv_queue fd=%d state=%s id=0x%02X%s%s len=%zu out_pending=%zu",
                     (long long)task->enqueue_ms, c->fd,
                     state_name(c->state), frame.packet_id, packet_name ? " " : "", packet_name ? packet_name : "",
                     frame.payload.len, out_pending);
        }
        conn_acquire(c);
        if (!mc_task_queue_push(&s->task_queue, task)) {
            log_error("task queue full; closing connection fd=%d", c->fd);
            conn_release(c);
            free(task->payload);
            free(task);
            return -1;
        }
        atomic_fetch_add(&worker->packets, 1);
        frame.payload.data = NULL;
        frame.payload.len = 0;
        memset(&frame, 0, sizeof(frame));
    }

    }
}

static int handle_client_write(mc_conn_t *c) {
    int64_t start_ms = debug_net_latency_enabled() ? now_ms() : 0;
    size_t before_pending = 0;
    size_t flushed = 0;
    pthread_mutex_lock(&c->out_lock);
    if (c->abort_io || c->fd < 0) { pthread_mutex_unlock(&c->out_lock); return -1; }
    before_pending = c->out.len > c->out.rpos ? c->out.len - c->out.rpos : 0;
    while (c->out.rpos < c->out.len && flushed < MAX_WRITE_BYTES_PER_PASS) {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        size_t amount = c->out.len - c->out.rpos;
        if (amount > MAX_WRITE_BYTES_PER_PASS - flushed) amount = MAX_WRITE_BYTES_PER_PASS - flushed;
        ssize_t n = send(c->fd, c->out.data + c->out.rpos, amount, flags);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            pthread_mutex_unlock(&c->out_lock);
            return -1;
        }
        if (n == 0) { pthread_mutex_unlock(&c->out_lock); return -1; }
        c->out.rpos += (size_t)n;
        flushed += (size_t)n;
        atomic_fetch_add(&c->server->workers[c->network_worker_index].bytes_out, (uint64_t)n);
    }
    buf_compact(&c->out);
    size_t remaining = c->out.len > c->out.rpos ? c->out.len - c->out.rpos : 0;
    if (debug_net_latency_enabled() && (before_pending > 0 || flushed > 0)) {
        log_info("net latency: t=%lld flush fd=%d before=%zu flushed=%zu remaining=%zu",
                 (long long)start_ms, c->fd,
                 before_pending, flushed, remaining);
    }
    if (c->server) {
        struct epoll_event ev = {0};
        ev.data.ptr = c;
        ev.events = EPOLLIN | EPOLLET | (remaining ? EPOLLOUT : 0);
        if (epoll_ctl(c->server->workers[c->network_worker_index].epoll_fd, EPOLL_CTL_MOD, c->fd, &ev) != 0) {
            pthread_mutex_unlock(&c->out_lock);
            return -1;
        }
    }
    if (c->closing && c->out.len == 0) {
        pthread_mutex_unlock(&c->out_lock);
        return -1;
    }
    pthread_mutex_unlock(&c->out_lock);
    return 0;
}

static int process_task(mc_server_t *s, mc_task_t *task, int64_t now_ms_value) {
    if (!task) return 0;
    mc_conn_t *c = task->conn;
    if (!c) return 0;
    if (c->closing) return 0;
    bool perf = mc_perf_enabled();
    int64_t handler_start_us = perf ? mc_now_us() : 0;
    mc_proto_state_t profile_state = perf ? (mc_proto_state_t)atomic_load(&c->state) : MC_STATE_HANDSHAKING;

    mc_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.packet_id = task->packet_id;
    frame.payload.data = task->payload;
    frame.payload.len = task->payload_len;
    frame.payload.cap = task->payload_len;
    frame.payload.rpos = 0;

    int rc = 0;
    int64_t process_start_ms = now_ms();
    if (debug_net_latency_enabled()) {
        const char *packet_name = serverbound_packet_name(c->state, frame.packet_id);
        int64_t wait_ms = task->enqueue_ms > 0 ? process_start_ms - task->enqueue_ms : 0;
        if (wait_ms < 0) wait_ms = 0;
        log_info("net latency: t=%lld process_task fd=%d player=%s state=%s id=0x%02X%s%s len=%zu queue_wait=%lldms",
                 (long long)process_start_ms, c->fd, c->username[0] ? c->username : "(unknown)",
                 state_name(c->state), frame.packet_id, packet_name ? " " : "", packet_name ? packet_name : "",
                 frame.payload.len, (long long)wait_ms);
    }
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
        if (proto_handle_status(c, &frame, "GoatCore 26.1.1", 0, 100) != 0) {
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
        int64_t packet_now_ms = task->enqueue_ms > 0 ? task->enqueue_ms : now_ms_value;
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

    if (perf && s) {
        int64_t elapsed_us = mc_now_us() - handler_start_us;
        s->task_handler_us += elapsed_us;
        s->task_handler_count++;
        if (elapsed_us > s->task_handler_max_us) s->task_handler_max_us = elapsed_us;
        if (profile_state == MC_STATE_PLAY && task->packet_id >= 0 && task->packet_id < 128) {
            mc_packet_perf_t *entry = &s->play_packet_perf[task->packet_id];
            entry->count++;
            entry->total_us += elapsed_us;
            if (elapsed_us > entry->max_us) entry->max_us = elapsed_us;
        }
    }
    return rc;
}

static size_t server_broadcast_pending_updates(mc_server_t *s, mc_conn_snapshot_t *snapshot, const char *phase) {
    if (!s || !s->world || !snapshot) return 0;

    const mc_block_update_t *updates = NULL;
    size_t updates_len = 0;
    updates = mc_world_updates(s->world, &updates_len);
    if (!updates || updates_len == 0) return 0;

    size_t sends = 0;
    for (size_t ci = 0; ci < snapshot->len; ci++) {
        mc_conn_t *c = snapshot->items[ci];
        mc_proto_state_t state = (mc_proto_state_t)atomic_load(&c->state);
        bool closing = atomic_load(&c->closing);
        if (state != MC_STATE_PLAY || !c->play_ready || closing) continue;

        for (size_t i = 0; i < updates_len; i++) {
            const mc_block_update_t *u = &updates[i];
            if (debug_broadcast_enabled()) {
                log_info("broadcast debug: t=%lld phase=%s block_update player=%s pos=(%d,%d,%d) state=%d",
                         (long long)now_ms(), phase ? phase : "tick",
                         c->username[0] ? c->username : "(unknown)", u->x, u->y, u->z, u->state_id);
            }
            sends++;
            if (send_block_update(c, u->x, u->y, u->z, u->state_id) != 0) {
                conn_close(c);
                break;
            }
        }
    }

    mc_world_clear_updates(s->world);
    return sends;
}

static size_t server_sync_remote_players_snapshot(mc_conn_snapshot_t *snapshot, const char *phase) {
    if (!snapshot) return 0;

    size_t attempts = 0;
    for (size_t vi = 0; vi < snapshot->len; vi++) {
        mc_conn_t *viewer = snapshot->items[vi];
        if (atomic_load(&viewer->closing) || atomic_load(&viewer->state) != MC_STATE_PLAY || !viewer->play_ready) continue;
        for (size_t si = 0; si < snapshot->len; si++) {
            mc_conn_t *subject = snapshot->items[si];
            if (subject == viewer || atomic_load(&subject->closing) || atomic_load(&subject->state) != MC_STATE_PLAY ||
                !subject->play_ready || !subject->has_pos) {
                continue;
            }
            attempts++;
            if (debug_broadcast_enabled()) {
                log_info("broadcast debug: t=%lld phase=%s remote_sync viewer=%s subject=%s subject_eid=%d",
                         (long long)now_ms(), phase ? phase : "tick",
                         viewer->username[0] ? viewer->username : "(viewer)",
                         subject->username[0] ? subject->username : "(subject)", subject->entity_id);
            }
            if (proto_play_sync_remote_player(viewer, subject) != 0) {
                conn_close(viewer);
                break;
            }
        }
    }
    return attempts;
}

static size_t network_flush_pending(mc_network_worker_t *worker) {
    if (!worker) return 0;
    mc_server_t *s = worker->server;
    size_t flushed_conns = 0;
    mc_conn_t *c = worker->owned;
    while (c) {
        mc_conn_t *next = c->owner_next;
        pthread_mutex_lock(&c->out_lock);
        bool had_pending = c->out.len > c->out.rpos;
        pthread_mutex_unlock(&c->out_lock);
        if (had_pending) {
            flushed_conns++;
            if (handle_client_write(c) != 0) server_close_conn(s, c);
        }
        c = next;
    }
    return flushed_conns;
}

static size_t server_process_queued_tasks(mc_server_t *s, int64_t now, const char *phase) {
    if (!s) return 0;

    size_t task_count = 0;
    mc_task_t *task = mc_task_queue_drain(&s->task_queue);
    while (task) {
        mc_task_t *next = task->next;
        task_count++;
        network_worker_record_task_wait(s, task, now);
        if (debug_net_latency_enabled()) {
            int64_t wait_ms = task->enqueue_ms > 0 ? now - task->enqueue_ms : 0;
            if (wait_ms < 0) wait_ms = 0;
            log_info("net latency: t=%lld task_drain phase=%s id=0x%02X wait=%lldms",
                     (long long)now, phase ? phase : "tick", task->packet_id, (long long)wait_ms);
        }
        if (task->type == MC_TASK_PACKET) {
            (void)process_task(s, task, now);
        }
        if (task->payload) free(task->payload);
        if (task->conn) conn_release(task->conn);
        free(task);
        task = next;
    }
    return task_count;
}

static void server_fast_packet_pass(mc_server_t *s, int64_t now, int64_t next_tick_us) {
    if (!s) return;

    int64_t start_ms = now_ms();
    int64_t start_us = mc_perf_enabled() ? mc_now_us() : 0;
    server_process_departures(s);
    size_t task_count = server_process_queued_tasks(s, now, "fast");
    if (task_count == 0) return;

    mc_conn_snapshot_t conn_snapshot = {0};
    size_t update_sends = 0;
    size_t remote_syncs = 0;
    if (server_snapshot_conns(s, &conn_snapshot) == 0) {
        update_sends = server_broadcast_pending_updates(s, &conn_snapshot, "fast");
        /* Movement smoothing is deliberately kept on the fixed 20 TPS tick.
         * The fast path remains for gameplay actions/block updates, but
         * per-packet movement broadcasts create irregular bursts
         * that look jittery on remote clients. */
        if (debug_broadcast_enabled()) {
            log_info("broadcast debug: t=%lld phase=fast remote_sync_deferred reason=stable_movement_tick",
                     (long long)now_ms());
        }
        conn_snapshot_clear(&conn_snapshot);
    } else {
        log_error("fast pass: failed to snapshot connections");
    }
    if (start_us) {
        s->fast_pass_us += mc_now_us() - start_us;
        s->fast_pass_tasks += task_count;
    }

    if (debug_net_latency_enabled() || debug_broadcast_enabled()) {
        log_info("net latency: t=%lld fast_pass tasks=%zu update_sends=%zu remote_syncs=%zu duration=%lldms next_tick_in=%lldms",
                 (long long)now_ms(), task_count, update_sends, remote_syncs,
                 (long long)(now_ms() - start_ms), (long long)((next_tick_us / 1000) - now_ms()));
    }
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
    perf_accum_t summary_handlers = {0};
    perf_accum_t summary_fast = {0};
    perf_accum_t summary_world_save = {0};
    perf_accum_t summary_flush = {0};
    perf_accum_t summary_item_lock_wait = {0};
    uint64_t summary_task_count = 0;
    uint64_t summary_update_sends = 0;
    uint64_t summary_proto_ticks = 0;
    uint64_t summary_remote_syncs = 0;
    uint64_t summary_resyncs = 0;
    uint32_t summary_tick_bins[PERF_TICK_BIN_MAX + 1] = {0};
    uint64_t summary_ticks_over_budget = 0;
    int64_t summary_wall_start_us = 0;
    int64_t summary_cpu_start_us = 0;

    while (atomic_load(&s->running)) {
        server_process_departures(s);
        bool perf = mc_perf_enabled();
        int64_t tick_start_us = 0;
        int64_t tick_start_cpu_us = 0;
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
        int64_t furnace_lock_wait_us = 0;
        int64_t items_lock_wait_us = 0;
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
            sleep_planned_us = next_tick_us - now_us_value;
            bool woke_for_task = mc_task_queue_wait_until(&s->task_queue, next_tick_us);
            now_us_value = mc_now_us();
            if (woke_for_task && now_us_value < next_tick_us && atomic_load(&s->running)) {
                if (debug_net_latency_enabled()) {
                    log_info("net latency: t=%lld wake_fast remaining_to_tick=%lldms",
                             (long long)(now_us_value / 1000), (long long)((next_tick_us - now_us_value) / 1000));
                }
                server_fast_packet_pass(s, now_us_value / 1000, next_tick_us);
                continue;
            }
        }
        if (now_us_value > next_tick_us) tick_late_us = now_us_value - next_tick_us;

        int64_t now = now_us_value / 1000;

        if (perf) {
            tick_start_us = now_us_value;
            tick_start_cpu_us = tick_thread_cpu_us();
        }
        int64_t span_start_us = perf ? mc_now_us() : 0;
        /* Packet handlers run on the tick thread via the task queue so they can
         * safely touch world state without fighting the epoll loop. */
        mc_task_t *task = mc_task_queue_drain(&s->task_queue);
        while (task) {
            mc_task_t *next = task->next;
            task_count++;
            network_worker_record_task_wait(s, task, now);
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

            /* Furnace ticking consults open-container state, so it briefly
             * shares conns_lock with the connection list. */
            int64_t lock_start_us = perf ? mc_now_us() : 0;
            pthread_mutex_lock(&s->conns_lock);
            if (perf) furnace_lock_wait_us = mc_now_us() - lock_start_us;
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

        /* Everything below can take time (chunk sends, protocol ticks, remote
         * player sync), so operate on the snapshot instead of the live list. */
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
                        int64_t key = (int64_t)(((uint64_t)(uint32_t)cx << 32) | (uint32_t)cz);
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
                    if (debug_broadcast_enabled()) {
                        log_info("broadcast debug: t=%lld phase=tick block_update player=%s pos=(%d,%d,%d) state=%d",
                                 (long long)now_ms(), c->username[0] ? c->username : "(unknown)",
                                 u->x, u->y, u->z, u->state_id);
                    }
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

        int64_t items_lock_start_us = perf ? mc_now_us() : 0;
        pthread_mutex_lock(&s->conns_lock);
        if (perf) items_lock_wait_us = mc_now_us() - items_lock_start_us;
        span_start_us = perf ? mc_now_us() : 0;
        item_entities_tick(s, now);
        if (perf) item_entities_us = mc_now_us() - span_start_us;
        pthread_mutex_unlock(&s->conns_lock);

        span_start_us = perf ? mc_now_us() : 0;
        remote_sync_attempts = server_sync_remote_players_snapshot(&conn_snapshot, "tick");
        if (perf) remote_sync_us = mc_now_us() - span_start_us;

        write_flush_count = (size_t)atomic_exchange(&s->network_flush_count, 0);
        write_flush_us = (int64_t)atomic_exchange(&s->network_write_us, 0);
        conn_snapshot_clear(&conn_snapshot);

        if (do_evict && s->world) {
            span_start_us = perf ? mc_now_us() : 0;
            evicted_count = mc_world_evict_outside(s->world, keep_keys, keep_len, WORLD_EVICT_BUDGET);
            if (perf) evict_us = mc_now_us() - span_start_us;
        }
        free(keep_keys);

        if (s->world && updates_len > 0) {
            span_start_us = perf ? mc_now_us() : 0;
            /* Clear only after every live PLAY connection had a chance to see
             * the coalesced updates for this tick. */
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
                        remote_sync_us >= slow_us || evict_us >= slow_us || clear_updates_us >= slow_us ||
                        tick_late_us >= slow_us || task_wait_max_us >= slow_us ||
                        items_lock_wait_us >= slow_us || furnace_lock_wait_us >= slow_us;
            if (slow) {
                log_info("perf tick: idx=%llu total=%.3fms late=%.3fms sleep=%.3fms tasks=%.3fms task_wait_avg=%.3fms task_wait_max=%.3fms world=%.3fms furnaces=%.3fms updates=%.3fms proto=%.3fms items=%.3fms remote=%.3fms net_write=%.3fms evict=%.3fms clear=%.3fms task_count=%zu conns=%zu ready=%zu updates_len=%zu update_sends=%zu proto_ticks=%zu remote_syncs=%zu net_flushes=%zu item_entities=%zu keep=%zu evicted=%zu world_done=%zu world_integrated=%zu world_dirty=%zu world_saves=%zu/%zu/%zu world_jobs=%zu furnaces_scanned=%zu furnaces_ticked=%zu furnaces_changed=%zu furnaces_errors=%zu",
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
                log_info("perf tick_detail: idx=%llu handlers=%.3fms handlers_max=%.3fms handlers_count=%zu fast_pass=%.3fms fast_tasks=%zu world_integrate=%.3fms world_save=%.3fms world_save_max=%.3fms world_done_lock_wait=%.3fms flush=%.3fms flush_max=%.3fms flush_count=%zu furnace_lock_wait=%.3fms items_lock_wait=%.3fms",
                         (unsigned long long)tick_index,
                         perf_ms(s->task_handler_us), perf_ms(s->task_handler_max_us), s->task_handler_count,
                         perf_ms(s->fast_pass_us), s->fast_pass_tasks,
                         perf_ms(world_stats.integrate_us), perf_ms(world_stats.save_us), perf_ms(world_stats.save_max_us),
                         perf_ms(world_stats.done_lock_wait_us), perf_ms(world_stats.flush_us),
                         perf_ms(world_stats.flush_max_us), world_stats.flush_count,
                         perf_ms(furnace_lock_wait_us), perf_ms(items_lock_wait_us));
            }

            uint64_t summary_ticks = mc_perf_summary_ticks();
            if (summary_ticks > 0) {
                if (summary_total.count == 0) {
                    summary_wall_start_us = tick_start_us;
                    summary_cpu_start_us = tick_start_cpu_us;
                }
                perf_accum_add(&summary_total, total_us);
                size_t tick_bin = (size_t)((total_us < 0 ? 0 : total_us) / PERF_TICK_BIN_US);
                if (tick_bin > PERF_TICK_BIN_MAX) tick_bin = PERF_TICK_BIN_MAX;
                summary_tick_bins[tick_bin]++;
                if (total_us > tick_us) summary_ticks_over_budget++;
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
                perf_accum_add(&summary_handlers, s->task_handler_us);
                perf_accum_add(&summary_fast, s->fast_pass_us);
                perf_accum_add(&summary_world_save, world_stats.save_us);
                perf_accum_add(&summary_flush, world_stats.flush_us);
                perf_accum_add(&summary_item_lock_wait, items_lock_wait_us);
                summary_task_count += task_count;
                summary_update_sends += block_update_sends;
                summary_proto_ticks += proto_tick_count;
                summary_remote_syncs += remote_sync_attempts;
                if (schedule_resync) summary_resyncs++;
                if (summary_total.count >= summary_ticks) {
                    int64_t wall_us = tick_end_us - summary_wall_start_us + tick_us;
                    double measured_tps = wall_us > 0 ?
                        (double)summary_total.count * 1000000.0 / (double)wall_us : 0.0;
                    double tick_cpu_pct = wall_us > 0 && summary_cpu_start_us > 0 ?
                        100.0 * (double)(tick_thread_cpu_us() - summary_cpu_start_us) / (double)wall_us : 0.0;
                    if (measured_tps > 20.0) measured_tps = 20.0;
                    log_info("perf summary: ticks=%llu tps=%.2f avg_total=%.3fms p50_total=%.1fms p95_total=%.1fms p99_total=%.1fms max_total=%.3fms over_50ms=%llu avg_late=%.3fms max_late=%.3fms avg_tasks=%.3fms avg_world=%.3fms avg_furnaces=%.3fms avg_updates=%.3fms avg_proto=%.3fms avg_items=%.3fms avg_remote=%.3fms avg_net_write=%.3fms avg_evict=%.3fms tasks=%llu update_sends=%llu proto_ticks=%llu remote_syncs=%llu resyncs=%llu",
                             (unsigned long long)summary_total.count,
                             measured_tps,
                             perf_accum_avg_ms(&summary_total),
                             perf_tick_percentile_ms(summary_tick_bins, summary_total.count, 50),
                             perf_tick_percentile_ms(summary_tick_bins, summary_total.count, 95),
                             perf_tick_percentile_ms(summary_tick_bins, summary_total.count, 99),
                             perf_ms(summary_total.max_us),
                             (unsigned long long)summary_ticks_over_budget,
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
                    log_network_workers_stats(s);
                    mc_task_queue_stats_t queue_stats;
                    mc_task_queue_get_stats(&s->task_queue, &queue_stats);
                    log_info("perf detail_summary: tick_cpu=%.1f%% avg_handlers=%.3fms max_handlers=%.3fms avg_fast_pass=%.3fms avg_world_save=%.3fms max_world_save=%.3fms avg_flush=%.3fms max_flush=%.3fms avg_items_lock_wait=%.3fms task_queue_depth=%zu task_queue_peak=%zu task_pushes=%llu task_rejected=%llu task_push_wait=%.3fms task_push_hold=%.3fms task_drain_wait=%.3fms task_drain_hold=%.3fms",
                             tick_cpu_pct, perf_accum_avg_ms(&summary_handlers), perf_ms(summary_handlers.max_us),
                             perf_accum_avg_ms(&summary_fast), perf_accum_avg_ms(&summary_world_save),
                             perf_ms(summary_world_save.max_us), perf_accum_avg_ms(&summary_flush),
                             perf_ms(summary_flush.max_us), perf_accum_avg_ms(&summary_item_lock_wait),
                             queue_stats.depth, queue_stats.peak_depth,
                             (unsigned long long)queue_stats.pushes, (unsigned long long)queue_stats.rejected,
                             perf_ms((int64_t)queue_stats.push_wait_us), perf_ms((int64_t)queue_stats.push_hold_us),
                             perf_ms((int64_t)queue_stats.drain_wait_us), perf_ms((int64_t)queue_stats.drain_hold_us));
                    for (int rank = 0; rank < 5; rank++) {
                        int best = -1;
                        for (int id = 0; id < 128; id++) {
                            if (s->play_packet_perf[id].total_us <= 0) continue;
                            if (best < 0 || s->play_packet_perf[id].total_us > s->play_packet_perf[best].total_us) best = id;
                        }
                        if (best < 0) break;
                        mc_packet_perf_t *entry = &s->play_packet_perf[best];
                        const char *name = serverbound_packet_name(MC_STATE_PLAY, best);
                        log_info("perf packet: id=0x%02X name=%s count=%llu total=%.3fms max=%.3fms",
                                 best, name ? name : "(unknown)", (unsigned long long)entry->count,
                                 perf_ms(entry->total_us), perf_ms(entry->max_us));
                        entry->total_us = 0;
                    }
                    memset(s->play_packet_perf, 0, sizeof(s->play_packet_perf));
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
                    summary_handlers = (perf_accum_t){0};
                    summary_fast = (perf_accum_t){0};
                    summary_world_save = (perf_accum_t){0};
                    summary_flush = (perf_accum_t){0};
                    summary_item_lock_wait = (perf_accum_t){0};
                    summary_task_count = 0;
                    summary_update_sends = 0;
                    summary_proto_ticks = 0;
                    summary_remote_syncs = 0;
                    summary_resyncs = 0;
                    memset(summary_tick_bins, 0, sizeof(summary_tick_bins));
                    summary_ticks_over_budget = 0;
                }
            }
        }
        s->task_handler_us = 0;
        s->task_handler_max_us = 0;
        s->task_handler_count = 0;
        s->fast_pass_us = 0;
        s->fast_pass_tasks = 0;

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
    server_process_departures(s);
    return NULL;
}

static void worker_update_peak(atomic_size_t *peak, size_t value) {
    size_t old = atomic_load(peak);
    while (value > old && !atomic_compare_exchange_weak(peak, &old, value)) {}
}

static void network_worker_enqueue(mc_network_worker_t *worker, mc_conn_t *c) {
    pthread_mutex_lock(&worker->command_lock);
    c->pending_next = NULL;
    if (worker->command_tail) worker->command_tail->pending_next = c;
    else worker->command_head = c;
    worker->command_tail = c;
    worker->command_len++;
    worker_update_peak(&worker->commands_peak, worker->command_len);
    pthread_mutex_unlock(&worker->command_lock);
    wake_eventfd(worker->wake_fd);
}

static void network_worker_drain_commands(mc_network_worker_t *worker, bool stop) {
    pthread_mutex_lock(&worker->command_lock);
    mc_conn_t *list = worker->command_head;
    worker->command_head = NULL;
    worker->command_tail = NULL;
    worker->command_len = 0;
    pthread_mutex_unlock(&worker->command_lock);
    while (list) {
        mc_conn_t *c = list;
        list = c->pending_next;
        c->pending_next = NULL;
        if (stop || add_epoll(worker->epoll_fd, c->fd, EPOLLIN | EPOLLET | EPOLLRDHUP, c) != 0) {
            server_close_conn(worker->server, c);
            continue;
        }
        c->owner_registered = true;
        c->owner_next = worker->owned;
        worker->owned = c;
        size_t current = atomic_fetch_add(&worker->connections, 1) + 1;
        worker_update_peak(&worker->connections_peak, current);
        atomic_fetch_add(&worker->accepted, 1);
    }
}

static void server_cleanup(mc_network_worker_t *worker, bool force) {
    mc_server_t *s = worker->server;
    for (mc_conn_t *c = worker->owned; c;) {
        mc_conn_t *next = c->owner_next;
        if (!force && !atomic_load(&c->closing)) {
            c = next;
            continue;
        }
        pthread_mutex_lock(&c->out_lock);
        bool can_close = force || c->abort_io || (c->out.len == 0);
        pthread_mutex_unlock(&c->out_lock);
        if (can_close) server_close_conn(s, c);
        c = next;
    }
}

static void *network_worker_main(void *arg) {
    mc_network_worker_t *worker = (mc_network_worker_t *)arg;
    mc_server_t *s = worker->server;
    struct epoll_event events[MAX_EVENTS];
    while (!atomic_load(&s->stop_requested)) {
        int64_t wait_start_us = mc_now_us();
        int n = epoll_wait(worker->epoll_fd, events, MAX_EVENTS, 1000);
        atomic_fetch_add(&worker->epoll_wait_us, (uint64_t)(mc_now_us() - wait_start_us));
        atomic_fetch_add(&worker->epoll_calls, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            log_error("network worker %u epoll_wait failed (errno=%d %s)", worker->index, errno, strerror(errno));
            atomic_store(&s->network_error, true);
            atomic_store(&s->stop_requested, true);
            wake_eventfd(s->wake_fd);
            break;
        }
        for (int i = 0; i < n; i++) {
            if (events[i].data.ptr == worker) {
                uint64_t count;
                while (read(worker->wake_fd, &count, sizeof(count)) > 0) {}
                network_worker_drain_commands(worker, false);
                int64_t start_us = mc_now_us();
                size_t flushed = network_flush_pending(worker);
                atomic_fetch_add(&s->network_flush_count, flushed);
                atomic_fetch_add(&s->network_write_us, (uint64_t)(mc_now_us() - start_us));
                continue;
            }
            mc_conn_t *c = (mc_conn_t *)events[i].data.ptr;
            if (atomic_load(&c->detached)) continue;
            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                server_close_conn(s, c);
                continue;
            }
            if (events[i].events & EPOLLOUT) {
                int64_t start_us = mc_now_us();
                int rc = handle_client_write(c);
                atomic_fetch_add(&s->network_flush_count, 1);
                atomic_fetch_add(&s->network_write_us, (uint64_t)(mc_now_us() - start_us));
                if (rc != 0) { server_close_conn(s, c); continue; }
            }
            if (events[i].events & EPOLLIN) {
                if (handle_client_read(s, c) != 0) { server_close_conn(s, c); continue; }
            }
            if (events[i].events & EPOLLRDHUP) server_close_conn(s, c);
        }
        server_cleanup(worker, false);
        server_release_retired(worker);
    }
    network_worker_drain_commands(worker, true);
    server_cleanup(worker, true);
    server_release_retired(worker);
    return NULL;
}

int net_server_run(mc_server_t *s) {
    if (!s) return -1;
    sigset_t shutdown_signals;
    sigemptyset(&shutdown_signals);
    sigaddset(&shutdown_signals, SIGINT);
    sigaddset(&shutdown_signals, SIGTERM);
    s->signal_fd = signalfd(-1, &shutdown_signals, SFD_NONBLOCK | SFD_CLOEXEC);
    if (s->signal_fd < 0 || add_epoll(s->epoll_fd, s->signal_fd, EPOLLIN, &s->signal_fd) != 0) {
        log_error("failed to initialize shutdown signalfd (errno=%d %s)", errno, strerror(errno));
        return -1;
    }
    atomic_store(&s->stop_requested, false);
    atomic_store(&s->running, true);
    if (pthread_create(&s->tick_thread, NULL, tick_thread_main, s) != 0) {
        atomic_store(&s->running, false);
        return -1;
    }
    int started = 0;
    for (int i = 0; i < s->network_worker_count; i++) {
        s->workers[i].started_us = mc_now_us();
        if (pthread_create(&s->workers[i].thread, NULL, network_worker_main, &s->workers[i]) != 0) {
            atomic_store(&s->stop_requested, true);
            break;
        }
        started++;
    }
    if (started != s->network_worker_count) {
        for (int i = 0; i < started; i++) wake_eventfd(s->workers[i].wake_fd);
        for (int i = 0; i < started; i++) pthread_join(s->workers[i].thread, NULL);
        atomic_store(&s->running, false);
        pthread_join(s->tick_thread, NULL);
        return -1;
    }
    struct epoll_event events[MAX_EVENTS];

    while (!atomic_load(&s->stop_requested)) {
        int n = epoll_wait(s->epoll_fd, events, MAX_EVENTS, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            atomic_store(&s->network_error, true);
            atomic_store(&s->stop_requested, true);
            break;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.ptr == s) {
                while (!atomic_load(&s->stop_requested)) {
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
                    unsigned worker_index = s->next_worker++ % (unsigned)s->network_worker_count;
                    mc_conn_t *c = conn_create(s, cfd, worker_index);
                    if (!c) {
                        close(cfd);
                        continue;
                    }
                    network_worker_enqueue(&s->workers[worker_index], c);
                }
            } else if (events[i].data.ptr == &s->wake_fd) {
                uint64_t count;
                while (read(s->wake_fd, &count, sizeof(count)) > 0) {}
            } else if (events[i].data.ptr == &s->signal_fd) {
                struct signalfd_siginfo signal_info;
                while (read(s->signal_fd, &signal_info, sizeof(signal_info)) == sizeof(signal_info)) {
                    net_server_stop(s);
                }
            }
        }
    }

    atomic_store(&s->stop_requested, true);
    for (int i = 0; i < s->network_worker_count; i++) wake_eventfd(s->workers[i].wake_fd);
    for (int i = 0; i < s->network_worker_count; i++) pthread_join(s->workers[i].thread, NULL);
    atomic_store(&s->running, false);
    pthread_join(s->tick_thread, NULL);
    return atomic_load(&s->network_error) ? -1 : 0;
}

void net_server_stop(mc_server_t *s) {
    if (!s) return;
    atomic_store(&s->stop_requested, true);
    wake_eventfd(s->wake_fd);
    for (int i = 0; i < s->network_worker_count; i++) wake_eventfd(s->workers[i].wake_fd);
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
    for (int i = 0; i < s->network_worker_count; i++) server_release_retired(&s->workers[i]);
    c = s->departures;
    while (c) {
        mc_conn_t *next = c->departure_next;
        proto_play_conn_cleanup(c);
        conn_release(c);
        c = next;
    }
    close(s->epoll_fd);
    close(s->wake_fd);
    if (s->signal_fd >= 0) close(s->signal_fd);
    close(s->listen_fd);
    network_workers_destroy(s);
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

mc_conn_t *net_server_find_conn_by_entity_id(mc_server_t *server, int32_t entity_id) {
    if (!server || entity_id <= 0) return NULL;
    pthread_mutex_lock(&server->conns_lock);
    mc_conn_t *c = server->conns;
    while (c) {
        if (c->entity_id == entity_id) {
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

int net_server_broadcast_system_message(mc_server_t *server, const char *text) {
    if (!server || !text) return -1;

    int rc = 0;
    mc_conn_snapshot_t snapshot;
    if (server_snapshot_conns(server, &snapshot) != 0) return -1;

    for (size_t i = 0; i < snapshot.len; i++) {
        mc_conn_t *c = snapshot.items[i];
        if (!c || c->closing || c->state != MC_STATE_PLAY || !c->play_init_sent) continue;
        if (proto_play_send_system_message(c, text) != 0) {
            conn_close(c);
            rc = -1;
        }
    }

    conn_snapshot_clear(&snapshot);
    return rc;
}

static int spawn_item_drop_locked_full(mc_server_t *server, double x, double y, double z, double vx, double vy, double vz,
                                       const mc_slot_t *slot, int32_t pickup_delay_ticks) {
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
    entity.vx = vx;
    entity.vy = vy;
    entity.vz = vz;
    entity.expires_at_ms = now_ms() + ITEM_ENTITY_TTL_MS;
    entity.pickup_delay_ticks = pickup_delay_ticks > 0 ? pickup_delay_ticks : 0;
    if (mc_slot_copy(&entity.slot, slot) != 0) {
        return -1;
    }

    server->item_entities[server->item_entities_len++] = entity;
    mc_item_entity_t *stored = &server->item_entities[server->item_entities_len - 1];
    if (debug_items_enabled()) {
        const char *item_name = mc_minecraft_item_name(stored->slot.item_id);
        log_info("items debug: t=%lld spawn eid=%d item=%s(%d) count=%d pos=(%.3f,%.3f,%.3f) vel=(%.3f,%.3f,%.3f) pickup_delay=%d",
                 (long long)now_ms(), stored->entity_id, item_name ? item_name : "(unknown)", stored->slot.item_id, stored->slot.count,
                 stored->x, stored->y, stored->z, stored->vx, stored->vy, stored->vz, stored->pickup_delay_ticks);
    }
    for (mc_conn_t *c = server->conns; c; c = c->next) {
        if (c->closing || c->state != MC_STATE_PLAY || !c->play_ready) continue;
        if (send_item_entity_spawn(c, stored) != 0 ||
            send_item_entity_metadata(c, stored) != 0 ||
            (item_entity_has_motion(stored) && send_item_entity_velocity(c, stored) != 0)) {
            conn_close(c);
        } else if (debug_broadcast_enabled() || debug_items_enabled()) {
            log_info("broadcast debug: t=%lld item_spawn player=%s eid=%d item=%d count=%d",
                     (long long)now_ms(), c->username[0] ? c->username : "(unknown)",
                     stored->entity_id, stored->slot.item_id, stored->slot.count);
        }
    }
    return 0;
}

int net_server_spawn_item_drop(mc_server_t *server, double x, double y, double z, const mc_slot_t *slot) {
    return net_server_spawn_item_drop_with_motion(server, x, y, z, 0.0, 0.0, 0.0, slot, ITEM_ENTITY_DEFAULT_PICKUP_DELAY_TICKS);
}

int net_server_spawn_item_drop_locked(mc_server_t *server, double x, double y, double z, const mc_slot_t *slot) {
    return spawn_item_drop_locked_full(server, x, y, z, 0.0, 0.0, 0.0, slot, ITEM_ENTITY_DEFAULT_PICKUP_DELAY_TICKS);
}

int net_server_spawn_item_drop_with_pickup_delay(mc_server_t *server, double x, double y, double z, const mc_slot_t *slot,
                                                 int32_t pickup_delay_ticks) {
    return net_server_spawn_item_drop_with_motion(server, x, y, z, 0.0, 0.0, 0.0, slot, pickup_delay_ticks);
}

int net_server_spawn_item_drop_with_motion(mc_server_t *server, double x, double y, double z, double vx, double vy, double vz,
                                           const mc_slot_t *slot, int32_t pickup_delay_ticks) {
    if (!server || !slot || !slot->present || slot->count <= 0) return -1;
    pthread_mutex_lock(&server->conns_lock);
    int rc = spawn_item_drop_locked_full(server, x, y, z, vx, vy, vz, slot, pickup_delay_ticks);
    pthread_mutex_unlock(&server->conns_lock);
    return rc;
}

int net_server_spawn_item_drop_locked_with_pickup_delay(mc_server_t *server, double x, double y, double z, const mc_slot_t *slot,
                                                        int32_t pickup_delay_ticks) {
    return spawn_item_drop_locked_full(server, x, y, z, 0.0, 0.0, 0.0, slot, pickup_delay_ticks);
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
        if (send_item_entity_spawn(conn, item) != 0 ||
            send_item_entity_metadata(conn, item) != 0 ||
            (item_entity_has_motion(item) && send_item_entity_velocity(conn, item) != 0)) {
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
