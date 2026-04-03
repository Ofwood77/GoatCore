#include "mc_server.h"
#include "mc_net.h"
#include "mc_protocol.h"
#include "mc_task_queue.h"
#include "mc_util.h"
#include "generated_minecraft_ids.h"

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
#define WORLD_EVICT_PERIOD_TICKS 20
#define WORLD_EVICT_BUDGET 32
#define ITEM_ENTITY_TTL_MS 30000
#define ITEM_ENTITY_METADATA_SLOT_INDEX 8

typedef struct {
    int32_t entity_id;
    uint8_t uuid[16];
    double x;
    double y;
    double z;
    int64_t expires_at_ms;
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
    mc_world_t *world;
    mc_item_entity_t *item_entities;
    size_t item_entities_len;
    size_t item_entities_cap;
};

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

static void close_matching_container_window(mc_conn_t *c) {
    if (!c || !c->active_window.open || !c->active_window.container) return;
    uint8_t window_id = (uint8_t)c->active_window.window_id;
    mc_container_instance_clear(c->active_window.container);
    free(c->active_window.container);
    memset(&c->active_window, 0, sizeof(c->active_window));
    (void)conn_write_packet(c, MC_PKT_PLAY_CLIENTBOUND_CONTAINER_CLOSE, &window_id, 1, -1);
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
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (ts.tv_nsec / 1000000);
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

int net_server_init(mc_server_t **out, const mc_server_config_t *cfg) {
    if (!out || !cfg) return -1;

    mc_server_t *s = (mc_server_t *)calloc(1, sizeof(mc_server_t));
    if (!s) return -1;
    s->cfg = *cfg;
    s->next_entity_id = 1;
    s->conns = NULL;
    atomic_init(&s->running, false);
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

static void item_entities_tick(mc_server_t *s, int64_t now) {
    if (!s) return;
    size_t idx = 0;
    while (idx < s->item_entities_len) {
        if (s->item_entities[idx].expires_at_ms <= now) {
            destroy_item_entity_at(s, idx);
            continue;
        }
        idx++;
    }
}

static int handle_client_read(mc_server_t *s, mc_conn_t *c) {
    uint8_t buf[4096];
    int64_t now = now_ms();
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
            if (packet_name) log_info("recv state=%s id=0x%02X (%s) len=%zu", state_name(c->state), frame.packet_id, packet_name, frame.payload.len);
            else log_info("recv state=%s id=0x%02X len=%zu", state_name(c->state), frame.packet_id, frame.payload.len);
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
        task->enqueue_ms = now;
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
        if (proto_play_handle(c, &frame, now_ms) != 0) {
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

static void *tick_thread_main(void *arg) {
    mc_server_t *s = (mc_server_t *)arg;
    const int64_t tick_ms = 50;
    int64_t next_tick = now_ms();
    uint64_t tick_index = 0;

    while (atomic_load(&s->running)) {
        int64_t now = now_ms();
        if (now < next_tick) {
            struct timespec ts;
            int64_t sleep_ms = next_tick - now;
            ts.tv_sec = sleep_ms / 1000;
            ts.tv_nsec = (sleep_ms % 1000) * 1000000;
            nanosleep(&ts, NULL);
            now = now_ms();
        }

        mc_task_t *task = mc_task_queue_drain(&s->task_queue);
        while (task) {
            mc_task_t *next = task->next;
            if (task->type == MC_TASK_PACKET) {
                (void)process_task(s, task, now);
            }
            if (task->payload) free(task->payload);
            if (task->conn) conn_release(task->conn);
            free(task);
            task = next;
        }

        if (s->world) {
            mc_world_tick(s->world, now);
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

        pthread_mutex_lock(&s->conns_lock);
        for (mc_conn_t *c = s->conns; c; c = c->next) {
            if (do_evict && c->state == MC_STATE_PLAY && c->has_pos && !c->closing) {
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

            if (updates && updates_len > 0 && c->state == MC_STATE_PLAY && c->play_ready && !c->closing) {
                for (size_t i = 0; i < updates_len; i++) {
                    const mc_block_update_t *u = &updates[i];
                    if (send_block_update(c, u->x, u->y, u->z, u->state_id) != 0) {
                        conn_close(c);
                        break;
                    }
                }
            }
            if (c->state == MC_STATE_PLAY) {
                if (proto_play_tick(c, now) != 0) {
                    conn_close(c);
                }
            }
        }
        item_entities_tick(s, now);
        for (mc_conn_t *viewer = s->conns; viewer; viewer = viewer->next) {
            if (viewer->closing || viewer->state != MC_STATE_PLAY || !viewer->play_ready) continue;
            for (mc_conn_t *subject = s->conns; subject; subject = subject->next) {
                if (subject == viewer || subject->closing || subject->state != MC_STATE_PLAY || !subject->play_ready || !subject->has_pos) {
                    continue;
                }
                if (proto_play_sync_remote_player(viewer, subject) != 0) {
                    conn_close(viewer);
                    break;
                }
            }
        }
        for (mc_conn_t *c = s->conns; c; c = c->next) {
            if (handle_client_write(c) != 0) {
                conn_close(c);
            }
        }
        pthread_mutex_unlock(&s->conns_lock);

        if (do_evict && s->world) {
            (void)mc_world_evict_outside(s->world, keep_keys, keep_len, WORLD_EVICT_BUDGET);
        }
        free(keep_keys);

        if (s->world && updates_len > 0) {
            mc_world_clear_updates(s->world);
        }

        next_tick += tick_ms;
        if (now > next_tick + tick_ms) {
            next_tick = now + tick_ms;
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

int net_server_spawn_item_drop(mc_server_t *server, double x, double y, double z, const mc_slot_t *slot) {
    if (!server || !slot || !slot->present || slot->count <= 0) return -1;
    pthread_mutex_lock(&server->conns_lock);
    if (server->item_entities_len == server->item_entities_cap) {
        size_t new_cap = server->item_entities_cap ? server->item_entities_cap * 2 : 16;
        mc_item_entity_t *next = (mc_item_entity_t *)realloc(server->item_entities, new_cap * sizeof(*next));
        if (!next) {
            pthread_mutex_unlock(&server->conns_lock);
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
    entity.expires_at_ms = now_ms() + ITEM_ENTITY_TTL_MS;
    if (mc_slot_copy(&entity.slot, slot) != 0) {
        pthread_mutex_unlock(&server->conns_lock);
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
    pthread_mutex_unlock(&server->conns_lock);
    return 0;
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
        out->state_id = container->state_id;
        out->dirty = container->dirty;
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
