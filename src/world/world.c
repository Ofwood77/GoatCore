#define _GNU_SOURCE
#include "mc_atomic_file.h"
/*
 * World/chunk runtime, async chunk loading, and persistence glue.
 *
 * The important split is:
 * - worker threads load/generate chunks and push completed results
 * - the main world tick integrates those results, applies queued mutations,
 *   records block updates for networking, and performs bounded saves
 *
 * That separation keeps mutation order deterministic for gameplay code while
 * still allowing chunk IO and generation to happen off-thread.
 */
#include "mc_world.h"
#include "mc_block_neighbors.h"
#include "mc_hinged_blocks.h"
#include "mc_attached_blocks.h"
#include "mc_block_state.h"

#include "generated_registries.h"
#include "mc_chunk_store.h"
#include "mc_anvil.h"
#include "mc_furnace.h"
#include "mc_util.h"
#include "stb_perlin.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ANVIL_SAVE_ATTEMPTS_PER_TICK 1
#define MAX_WORKERS 16

typedef struct {
    int32_t x;
    int32_t y;
    int32_t z;
    int32_t state_id;
} mc_pending_mod_t;

/* Writes targeting a chunk that is still loading. They are replayed exactly
 * once when the chunk reaches READY, then converted into normal updates. */
typedef struct {
    mc_pending_mod_t *items;
    size_t len;
    size_t cap;
} mc_pending_mods_t;

typedef enum {
    CHUNK_SLOT_EMPTY = 0,
    CHUNK_SLOT_TOMB = 1,
    CHUNK_SLOT_LOADING = 2,
    CHUNK_SLOT_READY = 3
} mc_chunk_slot_state_t;

typedef struct {
    int64_t key;
    mc_chunk_slot_state_t state;
    mc_chunk_t *chunk;
    mc_pending_mods_t *pending;
} mc_chunk_entry_t;
/* One hash-slot in the chunk table. LOADING entries may accumulate pending
 * writes; READY entries point at the live chunk owned by the main thread. */

typedef struct {
    mc_chunk_entry_t *entries;
    size_t cap;
    size_t len;
    size_t tombs;
} mc_chunk_map_t;
/* Open-addressed map keyed by chunk coordinates. Tombs matter because chunks
 * are evicted over time, so removal cannot simply zero a slot. */

typedef struct {
    int32_t cx;
    int32_t cz;
    uint32_t priority;
} mc_chunk_job_t;

typedef struct mc_chunk_done {
    int64_t key;
    mc_chunk_t *chunk;
    mc_block_entity_store_t block_entities;
    bool generated;
    struct mc_chunk_done *next;
} mc_chunk_done_t;

#define NEIGHBOR_STACK_LIMIT 1024
#define NEIGHBOR_DEFER_LIMIT 4096
typedef struct {int32_t x,y,z;unsigned next;} mc_neighbor_frame_t;

/* World-owned runtime. The hash map tracks chunk lifecycle, chunk_list is the
 * stable iteration order for save/evict/tick passes, and updates is the
 * per-tick queue consumed by the networking layer. */
struct mc_world {
    char *world_path;
    mc_world_storage_t *storage;
    mc_save_mode_t save_mode;
    int save_error;
    bool saves_configured;
    mc_save_io_t *save_io;
    int64_t seed;

    float gen_base_freq;
    int32_t gen_base_y;
    int32_t gen_base_amp;

    mc_world_ids_t ids;
    int32_t cave_air;
    int32_t void_air;
    mc_block_entity_store_t block_entities;

    mc_chunk_map_t chunks;
    mc_chunk_t **chunk_list;
    size_t chunk_list_len;
    size_t chunk_list_cap;
    size_t save_cursor;

    mc_block_update_t *updates;
    size_t updates_len;
    size_t updates_cap;
    mc_environment_drop_t *environment_drops;
    size_t environment_drops_len,environment_drops_cap;

    /* Tick-owned local update stack, shared by doors and attached blocks. */
    mc_neighbor_frame_t neighbor_stack[NEIGHBOR_STACK_LIMIT];
    size_t neighbor_depth;
    bool neighbor_draining;
    mc_block_update_t deferred_neighbors[NEIGHBOR_DEFER_LIMIT];
    size_t deferred_neighbors_len,deferred_neighbors_head;

    int worker_count;
    pthread_t workers[MAX_WORKERS];

    pthread_mutex_t job_lock;
    pthread_cond_t job_cv;
    mc_chunk_job_t *jobs;
    size_t jobs_len;
    size_t jobs_cap;
    bool jobs_stop;

    pthread_mutex_t done_lock;
    mc_chunk_done_t *done_head;
    mc_chunk_done_t *done_tail;

    uint64_t tick_count;
    size_t flush_count; /* tick-owned MC_PERF samples since the previous world tick */
    int64_t flush_us;
    int64_t flush_max_us;

    bool debug_reload;
    int32_t debug_cx;
    int32_t debug_cz;
    int32_t debug_x;
    int32_t debug_y;
    int32_t debug_z;
    bool debug_containers;
    bool debug_container_pos_set;
    int32_t debug_container_x;
    int32_t debug_container_y;
    int32_t debug_container_z;
};

static uint64_t hash_u64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static int64_t chunk_key(int32_t cx, int32_t cz) {
    return (int64_t)(((uint64_t)(uint32_t)cx << 32) | (uint32_t)cz);
}

static int32_t floor_div_i32(int32_t a, int32_t b) {
    if (b == 0) return 0;
    if (a >= 0) return a / b;
    return -(((-a) + b - 1) / b);
}

int32_t mc_world_runtime_state_id_from_key(const char *key, int32_t fallback) {
    if (!key || !*key) return fallback;

    int32_t exact = mc_block_state_id(key, -1);
    if (exact >= 0) return exact;

    const char *facing = "north";
    const char *needle = strstr(key, "facing=");
    if (needle) {
        needle += 7;
        if (strncmp(needle, "south", 5) == 0) facing = "south";
        else if (strncmp(needle, "west", 4) == 0) facing = "west";
        else if (strncmp(needle, "east", 4) == 0) facing = "east";
    }

    char normalized[128];
    if (strncmp(key, "minecraft:chest[", 16) == 0) {
        snprintf(normalized, sizeof(normalized), "minecraft:chest[facing=%s,type=single,waterlogged=false]", facing);
        return mc_block_state_id(normalized, fallback);
    }
    if (strncmp(key, "minecraft:trapped_chest[", 24) == 0) {
        snprintf(normalized, sizeof(normalized), "minecraft:trapped_chest[facing=%s,type=single,waterlogged=false]", facing);
        return mc_block_state_id(normalized, fallback);
    }
    if (strncmp(key, "minecraft:ender_chest[", 22) == 0) {
        snprintf(normalized, sizeof(normalized), "minecraft:ender_chest[facing=%s,waterlogged=false]", facing);
        return mc_block_state_id(normalized, fallback);
    }

    const char *props = strchr(key, '[');
    if (props) {
        char base[128];
        size_t len = (size_t)(props - key);
        if (len > 0 && len < sizeof(base)) {
            memcpy(base, key, len);
            base[len] = '\0';
            int32_t base_id = mc_block_state_id(base, -1);
            if (base_id >= 0) return base_id;
        }
    }

    return fallback;
}

int32_t mc_world_normalize_container_state_id(int32_t state_id) {
    const char *key = NULL;

    /* Modern runtime/network state IDs from generated_registries are already
     * authoritative and must pass through unchanged. Re-normalizing them risks
     * remapping a valid 26.1 BlockState into a different state. */
    key = mc_block_state_key(state_id);
    if (key) return state_id;

    /* Only legacy/mock bootstrap IDs from the temporary global palette should
     * be rewritten into the real runtime registry. */
    if (state_id >= 0 && (size_t)state_id < GLOBAL_BLOCK_STATES_COUNT &&
        (GLOBAL_BLOCK_STATES[state_id].flags & MC_BLOCK_FLAG_VALID) != 0) {
        key = mc_global_state_key((mc_global_state_id_t)state_id);
    }
    if (!key) return state_id;
    return mc_world_runtime_state_id_from_key(key, state_id);
}

static bool normalize_chunk_container_states(mc_chunk_t *chunk) {
    if (!chunk) return false;
    bool changed = false;
    for (int y = MC_WORLD_MIN_Y; y < MC_WORLD_MIN_Y + MC_WORLD_HEIGHT; y++) {
        for (int z = 0; z < MC_CHUNK_XZ; z++) {
            for (int x = 0; x < MC_CHUNK_XZ; x++) {
                int32_t current = (int32_t)mc_chunk_get_block(chunk, x, y, z);
                int32_t normalized = mc_world_normalize_container_state_id(current);
                if (normalized != current && mc_chunk_set_block(chunk, x, y, z, (mc_global_state_id_t)normalized) == 0) {
                    changed = true;
                }
            }
        }
    }
    return changed;
}

static bool debug_target_chunk(const mc_world_t *w, int32_t cx, int32_t cz) {
    return w && w->debug_reload && w->debug_cx == cx && w->debug_cz == cz;
}

static void maybe_init_debug_reload(mc_world_t *w) {
    if (!w) return;
    const char *env = getenv("MC_DEBUG_CHUNK_RELOAD");
    if (!env || !*env) return;

    int32_t cx = 0, cz = 0, x = 0, y = 0, z = 0;
    if (sscanf(env, "%d,%d,%d,%d,%d", &cx, &cz, &x, &y, &z) != 5) {
        log_error("MC_DEBUG_CHUNK_RELOAD invalid, expected cx,cz,x,y,z got '%s'", env);
        return;
    }

    w->debug_reload = true;
    w->debug_cx = cx;
    w->debug_cz = cz;
    w->debug_x = x;
    w->debug_y = y;
    w->debug_z = z;
    log_info("chunk reload debug enabled for chunk=(%d,%d) block=(%d,%d,%d)", cx, cz, x, y, z);
}

static void maybe_init_debug_containers(mc_world_t *w) {
    if (!w) return;
    const char *env = getenv("MC_DEBUG_CONTAINERS");
    w->debug_containers = (env && *env && strcmp(env, "0") != 0);
    const char *pos_env = getenv("MC_DEBUG_CONTAINER_POS");
    if (!pos_env || !*pos_env) return;
    int32_t x = 0, y = 0, z = 0;
    if (sscanf(pos_env, "%d,%d,%d", &x, &y, &z) != 3) {
        log_error("MC_DEBUG_CONTAINER_POS invalid, expected x,y,z got '%s'", pos_env);
        return;
    }
    w->debug_container_pos_set = true;
    w->debug_container_x = x;
    w->debug_container_y = y;
    w->debug_container_z = z;
}

bool mc_world_debug_containers_enabled(const mc_world_t *w) {
    return w && w->debug_containers;
}

bool mc_world_debug_container_match(const mc_world_t *w, int32_t x, int32_t y, int32_t z) {
    if (!w || !w->debug_containers) return false;
    if (!w->debug_container_pos_set) return true;
    return w->debug_container_x == x && w->debug_container_y == y && w->debug_container_z == z;
}

mc_block_entity_t *mc_world_get_block_entity(mc_world_t *w, int32_t x, int32_t y, int32_t z) {
    if (!w) return NULL;
    return mc_be_store_get(&w->block_entities, (mc_pos_t){x, y, z});
}

int mc_world_put_block_entity(mc_world_t *w, int32_t x, int32_t y, int32_t z, const mc_block_entity_t *entity) {
    if (!w || !entity) return -1;
    if(mc_storage_is_native(w->storage)){errno=EOPNOTSUPP;return -1;}
    return mc_be_store_put(&w->block_entities, (mc_pos_t){x, y, z}, *entity) ? 0 : -1;
}

int mc_world_remove_block_entity(mc_world_t *w, int32_t x, int32_t y, int32_t z) {
    if (!w) return -1;
    if(mc_storage_is_native(w->storage)){errno=EOPNOTSUPP;return -1;}
    return mc_be_store_remove(&w->block_entities, (mc_pos_t){x, y, z}) ? 0 : 1;
}

static int coords_to_chunk(int32_t x, int32_t z, int32_t *out_cx, int32_t *out_cz, int *out_lx, int *out_lz) {
    if (!out_cx || !out_cz || !out_lx || !out_lz) return -1;
    int32_t cx = (x >= 0) ? (x / MC_CHUNK_XZ) : ((x - (MC_CHUNK_XZ - 1)) / MC_CHUNK_XZ);
    int32_t cz = (z >= 0) ? (z / MC_CHUNK_XZ) : ((z - (MC_CHUNK_XZ - 1)) / MC_CHUNK_XZ);
    int lx = x - cx * MC_CHUNK_XZ;
    int lz = z - cz * MC_CHUNK_XZ;
    if (lx < 0 || lx >= MC_CHUNK_XZ || lz < 0 || lz >= MC_CHUNK_XZ) return -1;
    *out_cx = cx;
    *out_cz = cz;
    *out_lx = lx;
    *out_lz = lz;
    return 0;
}


static int pending_mods_push(mc_pending_mods_t *p, mc_pending_mod_t m) {
    if (!p) return -1;
    if (p->len == p->cap) {
        size_t new_cap = p->cap ? p->cap * 2 : 16;
        if (new_cap < p->cap) return -1;
        mc_pending_mod_t *next = (mc_pending_mod_t *)realloc(p->items, new_cap * sizeof(*next));
        if (!next) return -1;
        p->items = next;
        p->cap = new_cap;
    }
    p->items[p->len++] = m;
    return 0;
}

static void pending_mods_free(mc_pending_mods_t *p) {
    if (!p) return;
    free(p->items);
    p->items = NULL;
    p->len = 0;
    p->cap = 0;
    free(p);
}

static int updates_push(mc_world_t *w, mc_block_update_t u) {
    if (!w) return -1;
    /* Multiple mutations can hit the same block inside one tick. Keep only the
     * latest state so clients see the final authoritative result, not a short
     * history of intermediate states. */
    for (size_t i = w->updates_len; i > 0; i--) {
        mc_block_update_t *existing = &w->updates[i - 1];
        if (existing->x == u.x && existing->y == u.y && existing->z == u.z) {
            existing->state_id = u.state_id;
            return 0;
        }
    }
    if (w->updates_len == w->updates_cap) {
        size_t new_cap = w->updates_cap ? w->updates_cap * 2 : 256;
        if (new_cap < w->updates_cap) return -1;
        mc_block_update_t *next = (mc_block_update_t *)realloc(w->updates, new_cap * sizeof(*next));
        if (!next) return -1;
        w->updates = next;
        w->updates_cap = new_cap;
    }
    w->updates[w->updates_len++] = u;
    return 0;
}

static size_t next_pow2(size_t v) {
    size_t x = 1;
    while (x < v) x <<= 1;
    return x;
}

static void chunk_map_free(mc_chunk_map_t *m) {
    if (!m) return;
    if (m->entries) {
        for (size_t i = 0; i < m->cap; i++) {
            mc_chunk_entry_t *e = &m->entries[i];
            if (e->state == CHUNK_SLOT_READY) {
                mc_chunk_destroy(e->chunk);
                free(e->chunk);
            }
            if (e->state == CHUNK_SLOT_LOADING && e->pending) {
                pending_mods_free(e->pending);
                e->pending = NULL;
            }
        }
    }
    free(m->entries);
    m->entries = NULL;
    m->cap = 0;
    m->len = 0;
    m->tombs = 0;
}

static int chunk_map_init(mc_chunk_map_t *m, size_t cap_hint) {
    if (!m) return -1;
    memset(m, 0, sizeof(*m));
    size_t cap = next_pow2(cap_hint ? cap_hint : 64);
    if (cap < 64) cap = 64;
    m->entries = (mc_chunk_entry_t *)calloc(cap, sizeof(*m->entries));
    if (!m->entries) return -1;
    m->cap = cap;
    return 0;
}

static mc_chunk_entry_t *chunk_map_find_slot(mc_chunk_map_t *m, int64_t key, bool for_insert) {
    if (!m || !m->entries || m->cap == 0) return NULL;
    size_t mask = m->cap - 1;
    size_t idx = (size_t)hash_u64((uint64_t)key) & mask;
    size_t first_tomb = (size_t)-1;
    for (;;) {
        mc_chunk_entry_t *e = &m->entries[idx];
        if (e->state == CHUNK_SLOT_EMPTY) {
            if (!for_insert) return NULL;
            if (first_tomb != (size_t)-1) return &m->entries[first_tomb];
            return e;
        }
        if (e->state == CHUNK_SLOT_TOMB) {
            if (for_insert && first_tomb == (size_t)-1) first_tomb = idx;
        } else if (e->key == key) {
            return e;
        }
        idx = (idx + 1) & mask;
    }
}

static int chunk_map_rehash(mc_chunk_map_t *m, size_t new_cap) {
    if (!m) return -1;
    mc_chunk_entry_t *old = m->entries;
    size_t old_cap = m->cap;

    mc_chunk_entry_t *next = (mc_chunk_entry_t *)calloc(new_cap, sizeof(*next));
    if (!next) return -1;

    m->entries = next;
    m->cap = new_cap;
    m->len = 0;
    m->tombs = 0;

    for (size_t i = 0; i < old_cap; i++) {
        mc_chunk_entry_t *e = &old[i];
        if (e->state != CHUNK_SLOT_LOADING && e->state != CHUNK_SLOT_READY) continue;

        mc_chunk_entry_t *dst = chunk_map_find_slot(m, e->key, true);
        if (!dst) continue;
        *dst = *e;
        m->len++;
    }

    free(old);
    return 0;
}

static int chunk_map_maybe_grow(mc_chunk_map_t *m) {
    if (!m) return -1;
    if (m->cap == 0) return -1;
    size_t used = m->len + m->tombs;
    if (used * 10 < m->cap * 7) return 0;
    size_t new_cap = m->cap * 2;
    if (new_cap < m->cap) return -1;
    return chunk_map_rehash(m, new_cap);
}

static mc_chunk_entry_t *chunk_map_get(mc_chunk_map_t *m, int64_t key) {
    return chunk_map_find_slot(m, key, false);
}

static mc_chunk_entry_t *chunk_map_put_loading(mc_chunk_map_t *m, int64_t key) {
    if (!m) return NULL;
    if (chunk_map_maybe_grow(m) != 0) return NULL;
    mc_chunk_entry_t *e = chunk_map_find_slot(m, key, true);
    if (!e) return NULL;
    if (e->state == CHUNK_SLOT_LOADING || e->state == CHUNK_SLOT_READY) return e;
    if (e->state == CHUNK_SLOT_TOMB) m->tombs--;
    e->key = key;
    e->state = CHUNK_SLOT_LOADING;
    e->chunk = NULL;
    e->pending = NULL;
    m->len++;
    return e;
}

static void chunk_list_remove(mc_world_t *w, mc_chunk_t *c) {
    if (!w || !c || w->chunk_list_len == 0) return;
    size_t idx = c->list_index;
    if (idx >= w->chunk_list_len) return;
    size_t last = w->chunk_list_len - 1;
    if (idx != last) {
        mc_chunk_t *swap = w->chunk_list[last];
        w->chunk_list[idx] = swap;
        swap->list_index = idx;
    }
    w->chunk_list_len--;
    if (w->save_cursor > idx) w->save_cursor--;
    if (w->save_cursor >= w->chunk_list_len) w->save_cursor = 0;
}

static void chunk_map_remove_key(mc_world_t *w, int64_t key) {
    if (!w) return;
    mc_chunk_entry_t *e = chunk_map_get(&w->chunks, key);
    if (!e || e->state == CHUNK_SLOT_EMPTY || e->state == CHUNK_SLOT_TOMB) return;

    if (e->state == CHUNK_SLOT_LOADING && e->pending) {
        pending_mods_free(e->pending);
        e->pending = NULL;
    }

    if (e->state == CHUNK_SLOT_READY && e->chunk) {
        chunk_list_remove(w, e->chunk);
        mc_chunk_destroy(e->chunk);
        free(e->chunk);
        e->chunk = NULL;
    }

    e->state = CHUNK_SLOT_TOMB;
    w->chunks.len--;
    w->chunks.tombs++;
}

static int chunk_list_add(mc_world_t *w, mc_chunk_t *c) {
    if (!w || !c) return -1;
    if (w->chunk_list_len == w->chunk_list_cap) {
        size_t new_cap = w->chunk_list_cap ? w->chunk_list_cap * 2 : 128;
        if (new_cap < w->chunk_list_cap) return -1;
        mc_chunk_t **next = (mc_chunk_t **)realloc(w->chunk_list, new_cap * sizeof(*next));
        if (!next) return -1;
        w->chunk_list = next;
        w->chunk_list_cap = new_cap;
    }
    c->list_index = w->chunk_list_len;
    w->chunk_list[w->chunk_list_len++] = c;
    return 0;
}

static void job_queue_push_locked(mc_world_t *w, mc_chunk_job_t job) {
    if (!w) return;
    if (w->jobs_len == w->jobs_cap) {
        size_t new_cap = w->jobs_cap ? w->jobs_cap * 2 : 256;
        mc_chunk_job_t *next = (mc_chunk_job_t *)realloc(w->jobs, new_cap * sizeof(*next));
        if (!next) return;
        w->jobs = next;
        w->jobs_cap = new_cap;
    }

    size_t i = w->jobs_len;
    while (i > 0 && w->jobs[i - 1].priority < job.priority) {
        w->jobs[i] = w->jobs[i - 1];
        i--;
    }
    w->jobs[i] = job;
    w->jobs_len++;
}

static bool job_queue_pop_locked(mc_world_t *w, mc_chunk_job_t *out) {
    if (!w || !out) return false;
    if (w->jobs_len == 0) return false;
    *out = w->jobs[w->jobs_len - 1];
    w->jobs_len--;
    return true;
}

static void done_queue_push(mc_world_t *w, mc_chunk_done_t *d) {
    if (!w || !d) return;
    pthread_mutex_lock(&w->done_lock);
    d->next = NULL;
    if (!w->done_tail) {
        w->done_head = w->done_tail = d;
    } else {
        w->done_tail->next = d;
        w->done_tail = d;
    }
    pthread_mutex_unlock(&w->done_lock);
}

static mc_chunk_done_t *done_queue_drain(mc_world_t *w, int64_t *lock_wait_us) {
    if (!w) return NULL;
    int64_t start_us = lock_wait_us ? mc_now_us() : 0;
    pthread_mutex_lock(&w->done_lock);
    if (lock_wait_us) *lock_wait_us += mc_now_us() - start_us;
    mc_chunk_done_t *head = w->done_head;
    w->done_head = NULL;
    w->done_tail = NULL;
    pthread_mutex_unlock(&w->done_lock);
    return head;
}

void mc_world_set_generation_params(mc_world_t *w, float freq, int32_t base_y, int32_t amp) {
    if (!w) return;
    if (!(freq > 0.0f)) freq = 0.002f;
    if (amp < 0) amp = 0;
    w->gen_base_freq = freq;
    w->gen_base_y = base_y;
    w->gen_base_amp = amp;
}

void mc_world_generate_chunk(mc_world_t *w, mc_chunk_t *chunk) {
    if (!w || !chunk) return;
    const int32_t min_y = MC_WORLD_MIN_Y;
    const int32_t max_y = MC_WORLD_MIN_Y + MC_WORLD_HEIGHT - 1;
    const int32_t bedrock_y = 0;
    const int32_t dirt_min_y = 1;
    const int32_t dirt_max_y = 62;
    const int32_t grass_y = 63;
    int32_t bedrock_id = mc_block_state_id("minecraft:bedrock", w->ids.stone);

    for (int lz = 0; lz < MC_CHUNK_XZ; lz++) {
        for (int lx = 0; lx < MC_CHUNK_XZ; lx++) {
            for (int32_t y = min_y; y <= max_y; y++) {
                int32_t sid = w->ids.air;
                if (y == bedrock_y) sid = bedrock_id;
                else if (y >= dirt_min_y && y <= dirt_max_y) sid = (w->ids.dirt >= 0) ? w->ids.dirt : w->ids.stone;
                else if (y == grass_y) sid = (w->ids.grass_block_snowy_false >= 0) ? w->ids.grass_block_snowy_false : w->ids.dirt;
                (void)mc_chunk_set_block(chunk, lx, y, lz, (mc_global_state_id_t)sid);
            }
        }
    }

    chunk->loaded = true;
}

static int save_chunk_to_store_with_entities(const mc_world_t *w, mc_chunk_t *chunk, const mc_block_entity_store_t *be_store) {
    if (!w || !chunk) return -1;
    if (!w->world_path || !*w->world_path) return -1;

    int rc = mc_storage_write(w->storage, chunk, be_store ? be_store : &w->block_entities);
    if (rc != 0) {
        int e = errno;
        log_error("chunk store save %s chunk=(%d,%d) world=%s (errno=%d %s)",
                  rc == MC_SAVE_DURABILITY_UNCERTAIN ? "durability uncertain after publication" : "failed (not acknowledged durable)",
                  chunk->cx, chunk->cz, w->world_path ? w->world_path : "(null)", e, strerror(e));
    } else if (w->debug_containers && w->debug_container_pos_set) {
        int32_t cx = 0, cz = 0;
        int lx = 0, lz = 0;
        if (coords_to_chunk(w->debug_container_x, w->debug_container_z, &cx, &cz, &lx, &lz) == 0 && cx == chunk->cx && cz == chunk->cz &&
            w->debug_container_y >= MC_WORLD_MIN_Y && w->debug_container_y < MC_WORLD_MIN_Y + MC_WORLD_HEIGHT) {
            int32_t sid = (int32_t)mc_chunk_get_block(chunk, lx, w->debug_container_y, lz);
            log_info("containers debug: save chunk=(%d,%d) pos=(%d,%d,%d) state_id=%d key=%s", chunk->cx, chunk->cz,
                     w->debug_container_x, w->debug_container_y, w->debug_container_z, sid,
                     mc_block_state_key(sid) ? mc_block_state_key(sid) : "(null)");
        }
    } else if (debug_target_chunk(w, chunk->cx, chunk->cz)) {
        int32_t x = w->debug_x;
        int32_t y = w->debug_y;
        int32_t z = w->debug_z;
        int32_t cx = 0, cz = 0;
        int lx = 0, lz = 0;
        if (coords_to_chunk(x, z, &cx, &cz, &lx, &lz) == 0 && cx == chunk->cx && cz == chunk->cz &&
            y >= MC_WORLD_MIN_Y && y < MC_WORLD_MIN_Y + MC_WORLD_HEIGHT) {
            int32_t sid = (int32_t)mc_chunk_get_block(chunk, lx, y, lz);
            log_info("chunk reload debug: save chunk=(%d,%d) path=%s/region/r.%d.%d.mca dirty=%d block=(%d,%d,%d) state_id=%d key=%s",
                     chunk->cx, chunk->cz, w->world_path, floor_div_i32(chunk->cx, 32), floor_div_i32(chunk->cz, 32), chunk->dirty ? 1 : 0, x, y, z, sid,
                     mc_block_state_key(sid) ? mc_block_state_key(sid) : "(null)");
        }
    }
    return rc;
}

static int save_chunk_to_store(const mc_world_t *w, mc_chunk_t *chunk) {
    return save_chunk_to_store_with_entities(w, chunk, &w->block_entities);
}

static int merge_loaded_block_entities(mc_world_t *w, mc_block_entity_store_t *loaded) {
    if (!w || !loaded) return -1;
    for (size_t i = 0; i < loaded->cap; i++) {
        if (!loaded->states || loaded->states[i] != 1) continue;
        /* Transfer ownership of slot component buffers from the worker-local
         * store into the tick-owned store. Workers must not mutate
         * w->block_entities directly while furnace ticking/container saves may
         * iterate it on the authoritative tick thread. */
        if (!mc_be_store_put(&w->block_entities, loaded->positions[i], loaded->entities[i])) return -1;
        memset(&loaded->entities[i], 0, sizeof(loaded->entities[i]));
        loaded->states[i] = 0;
        if (loaded->len > 0) loaded->len--;
    }
    return 0;
}

static void apply_pending_mods(mc_world_t *w, mc_chunk_t *chunk, mc_pending_mods_t *pending) {
    if (!w || !chunk || !pending) return;
    /* Pending mods were requested against a chunk before it was ready. Apply
     * them in enqueue order so the READY chunk reflects every earlier write the
     * server already accepted. */
    for (size_t i = 0; i < pending->len; i++) {
        mc_pending_mod_t m = pending->items[i];
        int32_t cx = 0, cz = 0;
        int lx = 0, lz = 0;
        if (coords_to_chunk(m.x, m.z, &cx, &cz, &lx, &lz) != 0) continue;
        if (cx != chunk->cx || cz != chunk->cz) continue;
        if (m.y < MC_WORLD_MIN_Y || m.y >= MC_WORLD_MIN_Y + MC_WORLD_HEIGHT) continue;
        if (mc_chunk_set_block(chunk, lx, m.y, lz, (mc_global_state_id_t)m.state_id) != 0) continue;
        chunk->dirty = true;
        (void)updates_push(w, (mc_block_update_t){m.x, m.y, m.z, m.state_id});
    }
}

static void *worker_main(void *arg) {
    mc_world_t *w = (mc_world_t *)arg;
    for (;;) {
        pthread_mutex_lock(&w->job_lock);
        while (w->jobs_len == 0 && !w->jobs_stop) {
            pthread_cond_wait(&w->job_cv, &w->job_lock);
        }
        if (w->jobs_len == 0 && w->jobs_stop) {
            pthread_mutex_unlock(&w->job_lock);
            break;
        }
        mc_chunk_job_t job;
        bool ok = job_queue_pop_locked(w, &job);
        pthread_mutex_unlock(&w->job_lock);
        if (!ok) continue;

        mc_chunk_t *chunk = (mc_chunk_t *)calloc(1, sizeof(*chunk));
        if (!chunk) continue;
        if (mc_chunk_init(chunk, job.cx, job.cz, (mc_global_state_id_t)w->ids.air) != 0) {
            free(chunk);
            continue;
        }

        /* Workers never publish chunks directly into the live world map. They
         * produce a fully initialized chunk, then hand it back through the done
         * queue so the main tick can integrate it deterministically. */
        bool generated = false;
        bool normalized_on_load = false;
        mc_block_entity_store_t loaded_block_entities;
        mc_be_store_init(&loaded_block_entities);

        int store_rc = mc_storage_read(w->storage, job.cx, job.cz, chunk, &loaded_block_entities);
        if (store_rc == 0) {
            bool normalized = normalize_chunk_container_states(chunk);
            if (w->debug_containers && w->debug_container_pos_set) {
                int32_t cx = 0, cz = 0;
                int lx = 0, lz = 0;
                if (coords_to_chunk(w->debug_container_x, w->debug_container_z, &cx, &cz, &lx, &lz) == 0 &&
                    cx == job.cx && cz == job.cz &&
                    w->debug_container_y >= MC_WORLD_MIN_Y && w->debug_container_y < MC_WORLD_MIN_Y + MC_WORLD_HEIGHT) {
                    int32_t sid = (int32_t)mc_chunk_get_block(chunk, lx, w->debug_container_y, lz);
                    log_info("containers debug: load source=chunkstore chunk=(%d,%d) pos=(%d,%d,%d) state_id=%d key=%s normalized=%d",
                             job.cx, job.cz, w->debug_container_x, w->debug_container_y, w->debug_container_z, sid,
                             mc_block_state_key(sid) ? mc_block_state_key(sid) : "(null)", normalized ? 1 : 0);
                }
            }
            if (normalized) {
                chunk->dirty = true;
                normalized_on_load = true;
                if (w->debug_containers) {
                    log_info("containers debug: chunk=(%d,%d) source=anvil normalized=1", job.cx, job.cz);
                }
            }
            if (debug_target_chunk(w, job.cx, job.cz)) {
                log_info("chunk reload debug: worker chunk=(%d,%d) source=anvil", job.cx, job.cz);
            }
        } else {
            if (store_rc == 1) {
                if (debug_target_chunk(w, job.cx, job.cz)) {
                    log_info("chunk reload debug: worker chunk=(%d,%d) source=generate reason=absent", job.cx, job.cz);
                }
                mc_world_generate_chunk(w, chunk);
                chunk->dirty = (w->world_path && *w->world_path);
                generated = true;
            } else {
                /* A read failure is not proof of absence. Generating here can
                 * erase a valid file after a transient EIO, or destroy evidence
                 * needed to recover an already damaged snapshot. Keep this slot
                 * unavailable until an operator repairs/restarts the world. */
                int saved = errno;
                log_error("chunk store load failed; file preserved, chunk unavailable until restart chunk=(%d,%d) world=%s errno=%d",
                          job.cx, job.cz, w->world_path ? w->world_path : "(null)", saved);
                mc_chunk_destroy(chunk);
                free(chunk);
                mc_be_store_destroy(&loaded_block_entities);
                continue;
            }
        }
        if (normalize_chunk_container_states(chunk)) {
            chunk->dirty = true;
            normalized_on_load = true;
        }
        if (normalized_on_load && w->world_path && *w->world_path) {
            if (save_chunk_to_store_with_entities(w, chunk, &loaded_block_entities) == 0) {
                chunk->dirty = false;
                if (w->debug_containers) {
                    log_info("containers debug: chunk=(%d,%d) normalized_save=1", job.cx, job.cz);
                }
            }
        }
        chunk->loaded = true;

        mc_chunk_done_t *d = (mc_chunk_done_t *)calloc(1, sizeof(*d));
        if (!d) {
            mc_chunk_destroy(chunk);
            free(chunk);
            mc_be_store_destroy(&loaded_block_entities);
            continue;
        }
        d->key = chunk_key(job.cx, job.cz);
        d->chunk = chunk;
        d->block_entities = loaded_block_entities;
        d->generated = generated;
        done_queue_push(w, d);
    }
    return NULL;
}

mc_world_t *mc_world_create_with_storage(const char *world_path, int64_t level_seed, int worker_count, mc_storage_kind_t storage) {
    if (worker_count < 0 || worker_count > MAX_WORKERS) return NULL;
    mc_world_t *w = (mc_world_t *)calloc(1, sizeof(*w));
    if (!w) return NULL;

    if (world_path && *world_path) {
        w->world_path = strdup(world_path);
        if (!w->world_path) {
            free(w);
            return NULL;
        }
    }
    if (mc_storage_open(w->world_path, storage, &w->storage) != 0) {
        int e = errno;
        log_error("world storage unavailable: %s (errno=%d %s)", w->world_path ? w->world_path : "(memory)", e, strerror(e));
        free(w->world_path);
        free(w);
        return NULL;
    }

    w->seed = level_seed;
    maybe_init_debug_reload(w);
    maybe_init_debug_containers(w);
    mc_world_set_generation_params(w, 0.002f, 64, 24);

    w->ids.air = mc_block_state_id("minecraft:air", 0);
    w->ids.stone = mc_block_state_id("minecraft:stone", 1);
    w->ids.dirt = mc_block_state_id("minecraft:dirt", w->ids.stone);
    w->ids.grass_block_snowy_false = mc_block_state_id("minecraft:grass_block[snowy=false]", w->ids.dirt);
    w->cave_air = mc_block_state_id("minecraft:cave_air", w->ids.air);
    w->void_air = mc_block_state_id("minecraft:void_air", w->ids.air);

    for (int i = 0; i < 16; i++) {
        char key[128];
        snprintf(key, sizeof(key), "minecraft:water[level=%d]", i);
        w->ids.water_level[i] = mc_block_state_id(key, -1);
        snprintf(key, sizeof(key), "minecraft:lava[level=%d]", i);
        w->ids.lava_level[i] = mc_block_state_id(key, -1);
        snprintf(key, sizeof(key),
                 "minecraft:fire[age=%d,east=false,north=false,south=false,up=true,west=false]", i);
        w->ids.fire_age[i] = mc_block_state_id(key, -1);
        snprintf(key, sizeof(key),
                 "minecraft:redstone_wire[east=side,north=side,power=%d,south=side,west=side]", i);
        w->ids.wire_power[i] = mc_block_state_id(key, -1);
    }
    w->ids.redstone_block = mc_block_state_id("minecraft:redstone_block", -1);
    w->ids.lamp_lit[0] = mc_block_state_id("minecraft:redstone_lamp[lit=false]", -1);
    w->ids.lamp_lit[1] = mc_block_state_id("minecraft:redstone_lamp[lit=true]", -1);
    mc_be_store_init(&w->block_entities);

    if (chunk_map_init(&w->chunks, 256) != 0) {
        mc_storage_close(w->storage);
        free(w->world_path);
        free(w);
        return NULL;
    }

    w->chunk_list = NULL;
    w->chunk_list_len = 0;
    w->chunk_list_cap = 0;
    w->save_cursor = 0;

    if (pthread_mutex_init(&w->job_lock, NULL) != 0 || pthread_cond_init(&w->job_cv, NULL) != 0 ||
        pthread_mutex_init(&w->done_lock, NULL) != 0) {
        chunk_map_free(&w->chunks);
        mc_storage_close(w->storage);
        free(w->world_path);
        free(w);
        return NULL;
    }

    cpu_set_t available;
    int cpus = sched_getaffinity(0, sizeof(available), &available) == 0 ? CPU_COUNT(&available) : 1;
    int wc = worker_count ? worker_count : ((cpus > 2) ? cpus - 2 : 1);
    if (wc < 1) wc = 1;
    if (!worker_count && wc > 4) wc = 4;
    w->worker_count = wc;

    for (int i = 0; i < w->worker_count; i++) {
        if (pthread_create(&w->workers[i], NULL, worker_main, w) != 0) {
            pthread_mutex_lock(&w->job_lock);
            w->jobs_stop = true;
            pthread_cond_broadcast(&w->job_cv);
            pthread_mutex_unlock(&w->job_lock);
            for (int j = 0; j < i; j++) pthread_join(w->workers[j], NULL);

            chunk_map_free(&w->chunks);
            free(w->chunk_list);
            free(w->updates);
            free(w->jobs);
            mc_storage_close(w->storage);
            free(w->world_path);

            pthread_mutex_destroy(&w->job_lock);
            pthread_cond_destroy(&w->job_cv);
            pthread_mutex_destroy(&w->done_lock);
            free(w);
            return NULL;
        }
    }

    w->tick_count = 0;
    return w;
}

mc_world_t *mc_world_create_with_workers(const char *world_path, int64_t level_seed, int worker_count) {
    return mc_world_create_with_storage(world_path,level_seed,worker_count,MC_STORAGE_BIN);
}

mc_world_t *mc_world_create(const char *world_path, int64_t level_seed) {
    return mc_world_create_with_workers(world_path, level_seed, 0);
}

static void reconcile_chunk_neighbors(mc_world_t *w, mc_chunk_t *chunk);
static int drain_supported_neighbors(mc_world_t *w);

int mc_world_worker_count(const mc_world_t *w) {
    return w ? w->worker_count : 0;
}

mc_save_mode_t mc_world_save_mode(const mc_world_t *w) {
    return w ? w->save_mode : MC_SAVE_IMMEDIATE;
}
int mc_world_configure_saves(mc_world_t *w, mc_save_mode_t mode) {
    if (!w || w->chunks.len || w->saves_configured ||
        (mode != MC_SAVE_IMMEDIATE && mode != MC_SAVE_BATCHED && mode != MC_SAVE_ASYNC) ||
        (mode != MC_SAVE_IMMEDIATE && !mc_storage_is_native(w->storage))) {
        errno = EINVAL;
        return -1;
    }
    if (mode == MC_SAVE_ASYNC && mc_save_io_create(w->storage, &w->save_io) != 0)
        return -1;
    w->save_mode = mode;
    w->saves_configured = true;
    return 0;
}

/* Tick owner captures and acknowledges. Pending chunks stay pinned until their
 * matching completion; the I/O owner sees only deep independent snapshots. */
static int save_ack(mc_world_t *w, mc_save_batch_t *batch, mc_world_tick_stats_t *stats) {
    if (!batch)
        return 0;
    mc_save_clock_t span = mc_save_profile_begin();
    int rc = 0;
    for (size_t i = 0; i < batch->count; i++) {
        mc_chunk_t *snap = &batch->chunks[i];
        mc_chunk_entry_t *e = chunk_map_get(&w->chunks, chunk_key(snap->cx, snap->cz));
        int r = batch->results[i];
        if (!e || !e->chunk || mc_chunk_complete_save(e->chunk, snap, r) != 0) {
            w->save_error = ESTALE;
            rc = -1;
            r = -1;
            log_error("save completion owner missing or stale chunk=(%d,%d)", snap->cx, snap->cz);
        } else if (r) {
            w->save_error = batch->errors[i];
            rc = -1;
            log_error("save batch %s chunk=(%d,%d) generation=%" PRIu64 " errno=%d; dirty retained",
                      r == MC_SAVE_DURABILITY_UNCERTAIN ? "durability uncertain" : "failed", snap->cx,
                      snap->cz, snap->save_generation, batch->errors[i]);
        } else if (e->chunk->evict_after_save && !e->chunk->dirty) {
            chunk_map_remove_key(w, e->key);
            if (stats)
                stats->evict_after_save_removed++;
        }
        if (stats) {
            stats->saves_attempted++;
            if (!r)
                stats->saves_succeeded++;
            else
                stats->saves_failed++;
        }
    }
    mc_save_profile_end("save_batch_done", 0, (int32_t)batch->count, span, batch->bytes, rc);
    mc_save_batch_destroy(batch);
    return rc;
}
static int save_group(mc_world_t *w, mc_world_tick_stats_t *stats) {
    if (w->save_io && mc_save_io_busy(w->save_io)) {
        mc_save_clock_t span = mc_save_profile_begin();
        mc_save_profile_end("save_backpressure", 0, 0, span, 0, 0);
        return 0;
    }
    bool dirty = false;
    for (size_t i = 0; i < w->chunk_list_len; i++)
        if (w->chunk_list[i]->dirty) {
            dirty = true;
            break;
        }
    if (!dirty)
        return 0;
    mc_save_batch_t *batch = calloc(1, sizeof(*batch));
    if (!batch) {
        w->save_error = errno;
        log_error("save batch allocation failed; dirty retained");
        return -1;
    }
    int64_t start = stats ? mc_now_us() : 0;
    for (unsigned priority = 0; priority < 2 && batch->count < MC_SAVE_BATCH_CHUNKS; priority++) {
        size_t scanned = 0;
        while (scanned < w->chunk_list_len && batch->count < MC_SAVE_BATCH_CHUNKS) {
            if (w->save_cursor >= w->chunk_list_len)
                w->save_cursor = 0;
            mc_chunk_t *c = w->chunk_list[w->save_cursor++];
            scanned++;
            if (stats)
                stats->saves_scanned++;
            if (!c || !c->dirty || c->save_pending || c->save_requested != (priority == 0))
                continue;
            if (mc_save_batch_add(batch, c) != 0) {
                if (batch->count && errno == EFBIG)
                    break;
                w->save_error = errno;
                log_error("save snapshot refused chunk=(%d,%d) errno=%d; dirty retained", c->cx, c->cz,
                          errno);
                break;
            }
            c->save_pending = true;
            c->inflight_generation = c->save_generation;
        }
        if (w->save_error)
            break;
    }
    if (!batch->count && !w->save_error) {
        w->save_error = ESTALE;
        log_error("save batch made no progress; unmatched pending generation retained");
    }
    int rc = w->save_error ? -1 : 0;
    if (batch->count) {
        mc_save_clock_t span = mc_save_profile_begin();
        mc_save_profile_end("save_batch_submit", 0, (int32_t)batch->count, span, batch->bytes, 0);
        if (w->save_io) {
            if (mc_save_io_submit(w->save_io, batch) == 0)
                batch = NULL;
            else {
                w->save_error = errno;
                rc = -1;
                for (size_t i = 0; i < batch->count; i++) {
                    mc_chunk_entry_t *e =
                        chunk_map_get(&w->chunks, chunk_key(batch->chunks[i].cx, batch->chunks[i].cz));
                    if (e && e->chunk)
                        e->chunk->save_pending = false;
                }
                log_error("save submit failed; ownership retained, dirty data not discarded");
            }
        } else {
            mc_save_batch_execute(w->storage, batch);
            if (save_ack(w, batch, stats) != 0)
                rc = -1;
            batch = NULL;
        }
    }
    if (stats) {
        int64_t elapsed = mc_now_us() - start;
        stats->save_us += elapsed;
        if (elapsed > stats->save_max_us)
            stats->save_max_us = elapsed;
    }
    mc_save_batch_destroy(batch);
    return rc;
}
int mc_world_flush_saves(mc_world_t *w) {
    if (!w || !w->world_path || !*w->world_path)
        return 0;
    if (w->save_mode == MC_SAVE_IMMEDIATE) {
        int rc = 0;
        for (size_t i = 0; i < w->chunk_list_len; i++) {
            mc_chunk_t *c = w->chunk_list[i];
            if (c && c->dirty) {
                if (save_chunk_to_store(w, c) == 0)
                    c->dirty = false;
                else
                    rc = -1;
            }
        }
        return rc;
    }
    if (w->save_io)
        (void)save_ack(w, mc_save_io_take(w->save_io, true), NULL);
    w->save_error = 0; /* One explicit retry; never a per-tick failure loop. */
    for (;;) {
        bool dirty = false;
        for (size_t i = 0; i < w->chunk_list_len; i++)
            if (w->chunk_list[i]->dirty) {
                dirty = true;
                break;
            }
        if (!dirty)
            return 0;
        if (save_group(w, NULL) != 0)
            return -1;
        if (w->save_io && save_ack(w, mc_save_io_take(w->save_io, true), NULL) != 0)
            return -1;
    }
}
int mc_world_destroy_checked(mc_world_t *w) {
    if (!w) return 0;
    mc_save_clock_t shutdown_span=mc_save_profile_begin();

    pthread_mutex_lock(&w->job_lock);
    w->jobs_stop = true;
    pthread_cond_broadcast(&w->job_cv);
    pthread_mutex_unlock(&w->job_lock);

    for (int i = 0; i < w->worker_count; i++) {
        if (w->workers[i]) pthread_join(w->workers[i], NULL);
    }

    mc_chunk_done_t *done = done_queue_drain(w, NULL);
    while (done) {
        mc_chunk_done_t *next = done->next;
        mc_chunk_entry_t *e = chunk_map_get(&w->chunks, done->key);
        if (e && e->state == CHUNK_SLOT_LOADING) {
            e->chunk = done->chunk;
            e->state = CHUNK_SLOT_READY;
            if (chunk_list_add(w, done->chunk) != 0) {
                mc_chunk_destroy(done->chunk);
                free(done->chunk);
                e->chunk = NULL;
                e->state = CHUNK_SLOT_TOMB;
                w->chunks.len--;
                w->chunks.tombs++;
            } else if (e->pending) {
                if (merge_loaded_block_entities(w, &done->block_entities) != 0) {
                    log_error("failed to merge block entities for chunk key=%" PRId64, done->key);
                }
                apply_pending_mods(w, done->chunk, e->pending);
                pending_mods_free(e->pending);
                e->pending = NULL;
            } else if (merge_loaded_block_entities(w, &done->block_entities) != 0) {
                log_error("failed to merge block entities for chunk key=%" PRId64, done->key);
            }
        } else {
            mc_chunk_destroy(done->chunk);
            free(done->chunk);
        }
        if (e && e->state == CHUNK_SLOT_READY && e->chunk) reconcile_chunk_neighbors(w, e->chunk);
        mc_be_store_destroy(&done->block_entities);
        free(done);
        done = next;
    }

    int storage_rc=mc_world_flush_saves(w);
    if(storage_rc!=0)log_error("world shutdown persistence FAILED; unsaved dirty data remain world=%s",w->world_path);
    if(w->save_io) {
        mc_save_batch_t *completion=NULL;
        if(mc_save_io_close(w->save_io,&completion)!=0) {
            log_error("save worker join failed; world resources retained to prevent use-after-free");
            return -1;
        }
        w->save_io=NULL;
        (void)save_ack(w,completion,NULL);
    }

    chunk_map_free(&w->chunks);
    free(w->chunk_list);
    if(w->neighbor_depth){log_error("shutdown contains incomplete support updates");storage_rc=-1;}
    free(w->updates);
    free(w->environment_drops);
    free(w->jobs);
    mc_be_store_destroy(&w->block_entities);
    if(mc_storage_close_checked(w->storage)!=0)storage_rc=-1;
    free(w->world_path);

    pthread_mutex_destroy(&w->job_lock);
    pthread_cond_destroy(&w->job_cv);
    pthread_mutex_destroy(&w->done_lock);

    free(w);
    mc_save_profile_end("world_shutdown_save",0,0,shutdown_span,0,storage_rc);
    return storage_rc;
}

void mc_world_destroy(mc_world_t *w) { (void)mc_world_destroy_checked(w); }

const char *mc_world_path(const mc_world_t *w) {
    return w ? w->world_path : NULL;
}

const mc_world_ids_t *mc_world_ids(const mc_world_t *w) {
    return w ? &w->ids : NULL;
}

mc_chunk_t *mc_world_get_chunk(mc_world_t *w, int32_t cx, int32_t cz, uint32_t priority) {
    if (!w) return NULL;
    int64_t key = chunk_key(cx, cz);
    mc_chunk_entry_t *e = chunk_map_get(&w->chunks, key);
    if (e && e->state == CHUNK_SLOT_READY) return e->chunk;
    if (e && e->state == CHUNK_SLOT_LOADING) return NULL;

    e = chunk_map_put_loading(&w->chunks, key);
    if (!e) return NULL;

    pthread_mutex_lock(&w->job_lock);
    if (!w->jobs_stop) {
        job_queue_push_locked(w, (mc_chunk_job_t){cx, cz, priority});
        pthread_cond_signal(&w->job_cv);
    }
    pthread_mutex_unlock(&w->job_lock);
    return NULL;
}

int mc_world_get_block(mc_world_t *w, int32_t x, int32_t y, int32_t z, int32_t *out_state_id) {
    if (!w || !out_state_id) return -1;
    if (y < MC_WORLD_MIN_Y || y >= MC_WORLD_MIN_Y + MC_WORLD_HEIGHT) {
        *out_state_id = w->ids.air;
        return 0;
    }
    int32_t cx = 0, cz = 0;
    int lx = 0, lz = 0;
    if (coords_to_chunk(x, z, &cx, &cz, &lx, &lz) != 0) {
        *out_state_id = w->ids.air;
        return 0;
    }
    mc_chunk_t *chunk = mc_world_get_chunk(w, cx, cz, UINT32_MAX);
    if (!chunk) {
        *out_state_id = w->ids.air;
        return 0;
    }
    *out_state_id = (int32_t)mc_chunk_get_block(chunk, lx, y, lz);
    return 0;
}

int mc_world_get_block_ready(mc_world_t *w, int32_t x, int32_t y, int32_t z, int32_t *out_state_id) {
    if (!w || !out_state_id) return -1;
    if (y < MC_WORLD_MIN_Y || y >= MC_WORLD_MIN_Y + MC_WORLD_HEIGHT) {
        *out_state_id = w->ids.air;
        return 0;
    }
    int32_t cx = 0, cz = 0;
    int lx = 0, lz = 0;
    if (coords_to_chunk(x, z, &cx, &cz, &lx, &lz) != 0) {
        *out_state_id = w->ids.air;
        return 0;
    }
    /* Strict variant used by protocol code that must distinguish "air" from
     * "chunk not loaded yet". */
    mc_chunk_t *chunk = mc_world_get_chunk(w, cx, cz, UINT32_MAX);
    if (!chunk) return 1;
    *out_state_id = (int32_t)mc_chunk_get_block(chunk, lx, y, lz);
    return 0;
}

static int neighbor_read(void *ctx, int32_t x, int32_t y, int32_t z, int32_t *state) {
    mc_world_t *w = ctx;
    if (y < MC_WORLD_MIN_Y || y >= MC_WORLD_MIN_Y + MC_WORLD_HEIGHT) { *state = w->ids.air; return 0; }
    int32_t cx, cz; int lx, lz;
    if (coords_to_chunk(x, z, &cx, &cz, &lx, &lz) != 0) return -1;
    mc_chunk_entry_t *e = chunk_map_get(&w->chunks, chunk_key(cx, cz));
    if (!e || e->state != CHUNK_SLOT_READY || !e->chunk) return 1;
    *state = (int32_t)mc_chunk_get_block(e->chunk, lx, y, lz);
    return 0;
}
int mc_world_resolve_block_state(mc_world_t *w, int32_t x, int32_t y, int32_t z, int32_t state, int32_t *resolved) {
    return w ? mc_block_neighbor_state(neighbor_read, w, x, y, z, state, resolved) : -1;
}
static int reconcile_block(mc_world_t *w, int32_t x, int32_t y, int32_t z) {
    int32_t current, next;
    if (neighbor_read(w, x, y, z, &current) != 0 || !mc_stairs_is_state(current)) return 0;
    int rc = mc_world_resolve_block_state(w, x, y, z, current, &next);
    if (rc == 1) return 0; /* Keep the saved shape until the neighbor is READY. */
    if (rc || next == current) return rc;
    if(!mc_world_storage_action_allowed(w,x,y,z))return 0;
    int32_t cx, cz; int lx, lz;
    if (coords_to_chunk(x, z, &cx, &cz, &lx, &lz) != 0) return -1;
    mc_chunk_entry_t *e = chunk_map_get(&w->chunks, chunk_key(cx, cz));
    if (mc_chunk_set_block(e->chunk, lx, y, lz, (uint32_t)next) != 0) return -1;
    e->chunk->dirty = true;
    return updates_push(w, (mc_block_update_t){x, y, z, next});
}
static int reconcile_supported_neighbors(mc_world_t *w,int32_t x,int32_t y,int32_t z);
static int reconcile_local(mc_world_t *w, int32_t x, int32_t y, int32_t z) {
    static const int directions[5][2] = {{0,0},{0,-1},{1,0},{0,1},{-1,0}};
    mc_block_update_t changed[5];size_t count=0;
    for(size_t i=0;i<5;i++){
        int nx=x+directions[i][0],nz=z+directions[i][1];int32_t before,after;
        bool known=neighbor_read(w,nx,y,nz,&before)==0;
        if(reconcile_block(w,nx,y,nz))return -1;
        if(known&&!neighbor_read(w,nx,y,nz,&after)&&before!=after)
            changed[count++]=(mc_block_update_t){nx,y,nz,after};
    }
    /* A neighboring stair may lose a full side when its shape changes. Its
     * dependents need the same notifications as an explicitly changed block. */
    for(size_t i=0;i<count;i++)if(reconcile_supported_neighbors(w,changed[i].x,y,changed[i].z))return -1;
    return 0;
}
/* Missing chunks are not air. Retain the source notification and retry when
 * a chunk is integrated; bounded/deduplicated, with an explicit overflow error. */
static int defer_neighbors(mc_world_t *w,int32_t x,int32_t y,int32_t z){
    for(size_t i=0;i<w->deferred_neighbors_len;i++){
        const mc_block_update_t *p=&w->deferred_neighbors[(w->deferred_neighbors_head+i)%NEIGHBOR_DEFER_LIMIT];if(p->x==x&&p->y==y&&p->z==z)return 0;
    }
    if(w->deferred_neighbors_len==NEIGHBOR_DEFER_LIMIT){errno=ENOBUFS;log_error("deferred neighbor capacity exhausted; blocks preserved");return -1;}
    size_t slot=(w->deferred_neighbors_head+w->deferred_neighbors_len++)%NEIGHBOR_DEFER_LIMIT;
    w->deferred_neighbors[slot]=(mc_block_update_t){x,y,z,0};return 0;
}
static int reserve_environment_drop(mc_world_t *w){
    if(w->environment_drops_len<w->environment_drops_cap)return 0;
    size_t cap=w->environment_drops_cap?w->environment_drops_cap*2:64;
    if(cap<=w->environment_drops_len||cap>SIZE_MAX/sizeof(*w->environment_drops))return -1;
    void *p=realloc(w->environment_drops,cap*sizeof(*w->environment_drops));if(!p)return -1;
    w->environment_drops=p;w->environment_drops_cap=cap;return 0;
}
static int drain_supported_neighbors(mc_world_t *w){
    /* Vanilla UPDATE_SHAPE_ORDER. Children are processed before the next face,
     * without C recursion. No new worker or independent update scheduler. */
    static const int direction[]={4,5,2,3,0,1};
    static const int step[6][3]={{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};
    if(w->neighbor_draining||!w->neighbor_depth)return 0;
    mc_neighbor_frame_t origin=w->neighbor_stack[w->neighbor_depth-1];
    int32_t x=origin.x,z=origin.z;
    w->neighbor_draining=true;int result=0;unsigned work=0,removed=0;
    mc_save_clock_t span=mc_save_profile_begin();
    while(w->neighbor_depth){
        mc_neighbor_frame_t *frame=&w->neighbor_stack[w->neighbor_depth-1];
        if(frame->next==6){w->neighbor_depth--;continue;}
        if(++work>65536){result=-1;errno=ELOOP;log_error("neighbor work budget exhausted; pending updates retained");break;}
        int d=direction[frame->next++];int sx=frame->x,sy=frame->y,sz=frame->z;
        int nx=sx+step[d][0],ny=sy+step[d][1],nz=sz+step[d][2];
        int32_t state,next;int rc=neighbor_read(w,nx,ny,nz,&state);bool drop=false;
        if(rc==1){if(defer_neighbors(w,sx,sy,sz))result=-1;continue;}
        if(rc){result=-1;continue;}
        next=state;
        if(mc_door_is_state(state)&&d<2)
            rc=mc_door_neighbor_change(neighbor_read,w,nx,ny,nz,state,sy,&next,&drop);
        else if(mc_attached_support_direction(state)==(d^1)){
            bool valid=true;rc=mc_attached_survives(neighbor_read,w,nx,ny,nz,state,&valid);
            if(!rc&&!valid){
                /* Block.updateOrDestroy routes AIR shape results through
                 * Level.destroyBlock, which restores the old fluid state. */
                next=mc_hinged_property(state,"waterlogged","true")?w->ids.water_level[0]:w->ids.air;drop=true;
            }
        }else continue;
        if(rc==1){if(defer_neighbors(w,sx,sy,sz))result=-1;continue;}
        if(rc){result=-1;continue;}
        if(next==state)continue;
        if(!mc_world_storage_action_allowed(w,nx,ny,nz)){
            if(errno==EAGAIN&&defer_neighbors(w,sx,sy,sz))result=-1;
            continue; /* Preserve opaque/native storage guards. */
        }
        if(w->neighbor_depth==NEIGHBOR_STACK_LIMIT||(drop&&reserve_environment_drop(w))){
            frame->next--;result=-1;break;
        }
        if(mc_world_set_block(w,nx,ny,nz,next)){frame->next--;result=-1;break;}
        /* Set-block only enqueues its child; no drop can consume this slot
         * until the current iteration has recorded the event. */
        if(drop){w->environment_drops[w->environment_drops_len++]=(mc_environment_drop_t){nx,ny,nz,state};removed++;}
        int32_t cx,cz;int lx,lz;
        if(!coords_to_chunk(nx,nz,&cx,&cz,&lx,&lz)){
            mc_chunk_entry_t *e=chunk_map_get(&w->chunks,chunk_key(cx,cz));
            if(e&&e->chunk)e->chunk->save_requested=true;
        }
    }
    w->neighbor_draining=false;
    mc_save_profile_end("support_neighbors",x,z,span,work,result);
    if(removed)mc_save_profile_end("support_removed",x,z,mc_save_profile_begin(),removed,0);
    return result;
}
static int reconcile_supported_neighbors(mc_world_t *w,int32_t x,int32_t y,int32_t z){
    if(w->neighbor_depth==NEIGHBOR_STACK_LIMIT){errno=ENOBUFS;return -1;}
    w->neighbor_stack[w->neighbor_depth++]=(mc_neighbor_frame_t){x,y,z,0};
    return drain_supported_neighbors(w);
}
static void retry_deferred_neighbors(mc_world_t *w){
    /* A single pass: entries still waiting are appended to the following pass.
     * No support load or SQLite query is made by the support predicates. */
    size_t count=w->deferred_neighbors_len;
    for(size_t i=0;i<count;i++){
        mc_block_update_t p=w->deferred_neighbors[w->deferred_neighbors_head];
        w->deferred_neighbors_head=(w->deferred_neighbors_head+1)%NEIGHBOR_DEFER_LIMIT;w->deferred_neighbors_len--;
        if(reconcile_supported_neighbors(w,p.x,p.y,p.z))log_error("deferred neighbor update failed");
    }
}
const mc_environment_drop_t *mc_world_environment_drops(const mc_world_t *w,size_t *len){
    *len=w?w->environment_drops_len:0;return w?w->environment_drops:NULL;
}
void mc_world_consume_environment_drops(mc_world_t *w,size_t count){
    if(!w||count>w->environment_drops_len)return;
    w->environment_drops_len-=count;
    if(w->environment_drops_len)memmove(w->environment_drops,w->environment_drops+count,w->environment_drops_len*sizeof(*w->environment_drops));
}
int mc_world_set_block_pair(mc_world_t *w,int32_t x,int32_t y,int32_t z,int32_t lower,int32_t upper){
    if(!w||w->neighbor_depth>NEIGHBOR_STACK_LIMIT-2||y< -64||y>=319||!mc_block_state_desc(lower)||!mc_block_state_desc(upper)||
       !mc_world_storage_action_allowed(w,x,y,z)||!mc_world_storage_action_allowed(w,x,y+1,z))return -1;
    if(mc_storage_is_native(w->storage)&&(mc_native_state_sensitive((uint32_t)lower)||mc_native_state_sensitive((uint32_t)upper)))return -1;
    int32_t cx,cz;int lx,lz;if(coords_to_chunk(x,z,&cx,&cz,&lx,&lz))return -1;
    mc_chunk_entry_t *e=chunk_map_get(&w->chunks,chunk_key(cx,cz));
    if(!e||e->state!=CHUNK_SLOT_READY||!e->chunk){errno=EAGAIN;return -1;}
    if(w->updates_cap-w->updates_len<2){
        size_t cap=w->updates_cap?w->updates_cap*2:256;
        if(cap<w->updates_len+2||cap>SIZE_MAX/sizeof(*w->updates))return -1;
        void *p=realloc(w->updates,cap*sizeof(*w->updates));if(!p)return -1;
        w->updates=p;w->updates_cap=cap;
    }
    if(mc_chunk_set_column_pair(e->chunk,lx,y,lz,(uint32_t)lower,(uint32_t)upper))return -1;
    (void)updates_push(w,(mc_block_update_t){x,y,z,lower});
    (void)updates_push(w,(mc_block_update_t){x,y+1,z,upper});
    /* Both halves already exist before any neighbor can observe them. */
    if(reconcile_supported_neighbors(w,x,y,z)||reconcile_supported_neighbors(w,x,y+1,z))
        log_error("door neighbor reconciliation failed after paired mutation");
    return 0;
}
/* Reconcile loaded stairs when either side of a chunk boundary becomes READY.
 * Palette filtering avoids scanning sections without relevant block families. */
static void reconcile_loaded_neighbors(mc_world_t *w, mc_chunk_t *chunk) {
    if (!chunk) return;
    for (int s = 0; s < MC_WORLD_SECTION_COUNT; s++) {
        mc_paletted_container_t *sec = &chunk->sections[s];
        bool possible = sec->is_direct || (mc_stairs_is_state((int32_t)sec->uniform_state)||mc_attached_family((int32_t)sec->uniform_state));
        for (unsigned i = 0; !possible && i < sec->palette_len; i++) possible = mc_stairs_is_state((int32_t)sec->palette[i])||mc_attached_family((int32_t)sec->palette[i]);
        if (!possible) continue;
        for (int y = 0; y < 16; y++) for (int z = 0; z < 16; z++) for (int x = 0; x < 16; x++) {
            int wy = MC_WORLD_MIN_Y + s * 16 + y;
            int32_t state=(int32_t)mc_chunk_get_block(chunk,x,wy,z);
            int wx=chunk->cx*16+x,wz=chunk->cz*16+z;
            if(mc_stairs_is_state(state)){
                if(reconcile_local(w,wx,wy,wz))log_error("stair neighbor reconciliation failed");
            }else{
                int d=mc_attached_support_direction(state);if(d<0)continue;
                static const int step[6][3]={{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};
                bool valid=true;int rc=mc_attached_survives(neighbor_read,w,wx,wy,wz,state,&valid);
                int sx=wx+step[d][0],sy=wy+step[d][1],sz=wz+step[d][2];
                if(rc==1){if(defer_neighbors(w,sx,sy,sz))log_error("loaded support update could not be retained");}
                else if(rc||(!valid&&reconcile_supported_neighbors(w,sx,sy,sz)))log_error("loaded support reconciliation failed");
            }
        }
    }
}
static void reconcile_chunk_neighbors(mc_world_t *w, mc_chunk_t *chunk) {
    retry_deferred_neighbors(w);
    static const int directions[5][2] = {{0,0},{0,-1},{1,0},{0,1},{-1,0}};
    for (size_t i = 0; i < 5; i++) {
        mc_chunk_entry_t *e = chunk_map_get(&w->chunks, chunk_key(chunk->cx + directions[i][0], chunk->cz + directions[i][1]));
        if (e && e->state == CHUNK_SLOT_READY) reconcile_loaded_neighbors(w, e->chunk);
    }
}

bool mc_world_storage_action_allowed(mc_world_t *w, int32_t x, int32_t y, int32_t z) {
    if (!w)
        return false;
    if (!mc_storage_is_native(w->storage))
        return true;
    if (w->save_error) {errno=w->save_error;return false;}
    if (y < MC_WORLD_MIN_Y || y >= MC_WORLD_MIN_Y + MC_WORLD_HEIGHT)
        return false;
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++) {
            if (x <= INT32_MIN + 1 || x >= INT32_MAX - 1 || z <= INT32_MIN + 1 || z >= INT32_MAX - 1)
                return false;
            int32_t cx, cz;
            int lx, lz;
            if (coords_to_chunk(x + dx, z + dz, &cx, &cz, &lx, &lz) != 0)
                return false;
            mc_chunk_t *c = mc_world_get_chunk(w, cx, cz, 0);
            if (!c || c->storage_readonly) {
                errno = EAGAIN;
                return false;
            }
            if (dx == 0 && dz == 0 && c->storage_guard) {
                unsigned cell = (unsigned)(((y + 64) << 8) | (lz << 4) | lx);
                if (c->storage_guard[cell / 8] & (1u << (cell % 8))) {
                    errno = EOPNOTSUPP;
                    return false;
                }
            }
            for (int dy = -1; dy <= 1; dy++) {
                if (y + dy < -64 || y + dy >= 320)
                    continue;
                if (mc_native_state_sensitive(mc_chunk_get_block(c, lx, y + dy, lz))) {
                    errno = EOPNOTSUPP;
                    return false;
                }
            }
        }
    return true;
}
int mc_world_set_block(mc_world_t *w, int32_t x, int32_t y, int32_t z, int32_t state_id) {
    if (!w || w->neighbor_depth>=NEIGHBOR_STACK_LIMIT) return -1;
    if (y < MC_WORLD_MIN_Y || y >= MC_WORLD_MIN_Y + MC_WORLD_HEIGHT) return -1;
    state_id = mc_world_normalize_container_state_id(state_id);
    if(mc_storage_is_native(w->storage) && (mc_native_state_sensitive((uint32_t)state_id) || !mc_world_storage_action_allowed(w,x,y,z))) {errno=EOPNOTSUPP;return -1;}

    int32_t cx = 0, cz = 0;
    int lx = 0, lz = 0;
    if (coords_to_chunk(x, z, &cx, &cz, &lx, &lz) != 0) return -1;

    int64_t key = chunk_key(cx, cz);
    mc_chunk_entry_t *e = chunk_map_get(&w->chunks, key);
    if (e && e->state == CHUNK_SLOT_READY && e->chunk) {
        mc_chunk_t *chunk = e->chunk;
        int32_t old = (int32_t)mc_chunk_get_block(chunk, lx, y, lz);
        if (old == state_id) return 0;
        /* Reserve notifications before a support cascade mutates anything. */
        if(w->updates_len==w->updates_cap){
            size_t cap=w->updates_cap?w->updates_cap*2:256;
            if(cap<=w->updates_len||cap>SIZE_MAX/sizeof(*w->updates))return -1;
            void *p=realloc(w->updates,cap*sizeof(*w->updates));if(!p)return -1;
            w->updates=p;w->updates_cap=cap;
        }
        /* For READY chunks, mutate memory first and record a broadcast update
         * from that final state. Network code should never need to re-derive
         * what changed by re-reading the chunk later in the tick. */
        if (mc_chunk_set_block(chunk, lx, y, lz, (mc_global_state_id_t)state_id) != 0) return -1;
        chunk->dirty = true;
        if (mc_world_debug_container_match(w, x, y, z) &&
            (mc_world_normalize_container_state_id(old) != old || mc_world_normalize_container_state_id(state_id) == state_id)) {
            log_info("containers debug: set_block chunk=(%d,%d) pos=(%d,%d,%d) requested=%d normalized=%d old=%d old_key=%s new_key=%s",
                     cx, cz, x, y, z, state_id, mc_world_normalize_container_state_id(state_id), old,
                     mc_block_state_key(old) ? mc_block_state_key(old) : "(null)",
                     mc_block_state_key(state_id) ? mc_block_state_key(state_id) : "(null)");
        }
        if (updates_push(w, (mc_block_update_t){x, y, z, state_id}) != 0) return -1;
        if((mc_stairs_is_state(old)||mc_stairs_is_state(state_id))&&reconcile_local(w,x,y,z))return -1;
        return reconcile_supported_neighbors(w,x,y,z);
    }

    if (!e) {
        e = chunk_map_put_loading(&w->chunks, key);
        if (!e) return -1;
        pthread_mutex_lock(&w->job_lock);
        if (!w->jobs_stop) {
            job_queue_push_locked(w, (mc_chunk_job_t){cx, cz, 0});
            pthread_cond_signal(&w->job_cv);
        }
        pthread_mutex_unlock(&w->job_lock);
    }

    if (!e->pending) {
        e->pending = (mc_pending_mods_t *)calloc(1, sizeof(*e->pending));
        if (!e->pending) return -1;
    }
    /* Writes against non-ready chunks are accepted optimistically and replayed
     * once the worker hands the chunk back to the main thread. */
    return pending_mods_push(e->pending, (mc_pending_mod_t){x, y, z, state_id});
}

int mc_world_flush_block(mc_world_t *w, int32_t x, int32_t y, int32_t z) {
    if (!w) return -1;
    if (!w->world_path || !*w->world_path) return 0;
    if (y < MC_WORLD_MIN_Y || y >= MC_WORLD_MIN_Y + MC_WORLD_HEIGHT) return -1;
    if(w->save_mode!=MC_SAVE_IMMEDIATE)return mc_world_flush_saves(w);

    int32_t cx = 0, cz = 0;
    int lx = 0, lz = 0;
    if (coords_to_chunk(x, z, &cx, &cz, &lx, &lz) != 0) return -1;
    (void)lx;
    (void)lz;

    mc_chunk_t *chunk = mc_world_get_chunk(w, cx, cz, UINT32_MAX);
    if (!chunk) return -1;
    if (!chunk->dirty) return 0;
    bool perf = mc_perf_enabled();
    int64_t flush_start_us = perf ? mc_now_us() : 0;
    int save_rc = save_chunk_to_store(w, chunk);
    if (perf) {
        int64_t elapsed_us = mc_now_us() - flush_start_us;
        w->flush_count++;
        w->flush_us += elapsed_us;
        if (elapsed_us > w->flush_max_us) w->flush_max_us = elapsed_us;
    }
    if (save_rc != 0) return -1;
    chunk->dirty = false;
    if (mc_world_debug_container_match(w, x, y, z)) {
        int32_t sid = (int32_t)mc_chunk_get_block(chunk, lx, y, lz);
        log_info("containers debug: flush chunk=(%d,%d) block=(%d,%d,%d) state_id=%d key=%s", cx, cz, x, y, z, sid,
                 mc_block_state_key(sid) ? mc_block_state_key(sid) : "(null)");
    }
    return 0;
}

int mc_world_request_block_save(mc_world_t *w,int32_t x,int32_t y,int32_t z) {
    if(!w)return -1;
    if(w->save_mode==MC_SAVE_IMMEDIATE)return mc_world_flush_block(w,x,y,z);
    if(w->save_error){errno=w->save_error;return -1;}
    if(y<MC_WORLD_MIN_Y || y>=MC_WORLD_MIN_Y+MC_WORLD_HEIGHT){errno=EINVAL;return -1;}
    int32_t cx,cz;int lx,lz;
    if(coords_to_chunk(x,z,&cx,&cz,&lx,&lz)!=0)return -1;
    mc_chunk_t *c=mc_world_get_chunk(w,cx,cz,UINT32_MAX);
    if(!c){errno=EAGAIN;return -1;}
    if(c->dirty)c->save_requested=true;
    return 0;
}

int mc_world_mark_chunk_dirty_at(mc_world_t *w, int32_t x, int32_t z) {
    if (!w) return -1;
    int32_t cx = 0;
    int32_t cz = 0;
    int lx = 0;
    int lz = 0;
    if (coords_to_chunk(x, z, &cx, &cz, &lx, &lz) != 0) return -1;
    (void)lx;
    (void)lz;
    mc_chunk_t *chunk = mc_world_get_chunk(w, cx, cz, UINT32_MAX);
    if (!chunk) return -1;
    return mc_chunk_mark_dirty(chunk);
}

void mc_world_tick(mc_world_t *w, int64_t now_ms) {
    mc_world_tick_profiled(w, now_ms, NULL);
}

void mc_world_tick_profiled(mc_world_t *w, int64_t now_ms, mc_world_tick_stats_t *stats) {
    (void)now_ms;
    if (stats) memset(stats, 0, sizeof(*stats));
    if (!w) return;
    w->tick_count++;
    if(w->neighbor_depth&&drain_supported_neighbors(w))log_error("pending support update failed; retained for retry");
    if(w->save_io)(void)save_ack(w,mc_save_io_take(w->save_io,false),stats);
    if (stats) {
        stats->flush_count = w->flush_count;
        stats->flush_us = w->flush_us;
        stats->flush_max_us = w->flush_max_us;
    }
    w->flush_count = 0;
    w->flush_us = 0;
    w->flush_max_us = 0;

    /* 1) Integrate worker results and any writes that piled up while those
     * chunks were loading. Gameplay code after this point sees only READY
     * chunks or explicit "not ready" answers. */
    int64_t integration_start_us = stats ? mc_now_us() : 0;
    mc_chunk_done_t *done = done_queue_drain(w, stats ? &stats->done_lock_wait_us : NULL);
    while (done) {
        mc_chunk_done_t *next = done->next;
        if (stats) stats->done_seen++;
        mc_chunk_entry_t *e = chunk_map_get(&w->chunks, done->key);
        if (e && e->state == CHUNK_SLOT_LOADING) {
            e->chunk = done->chunk;
            e->state = CHUNK_SLOT_READY;
            if (chunk_list_add(w, done->chunk) != 0) {
                if (stats) stats->done_add_failed++;
                mc_chunk_destroy(done->chunk);
                free(done->chunk);
                e->chunk = NULL;
                e->state = CHUNK_SLOT_TOMB;
                w->chunks.len--;
                w->chunks.tombs++;
            } else if (e->pending) {
                if (stats) stats->done_integrated++;
                if (merge_loaded_block_entities(w, &done->block_entities) != 0) {
                    log_error("failed to merge block entities for chunk key=%" PRId64, done->key);
                }
                apply_pending_mods(w, done->chunk, e->pending);
                pending_mods_free(e->pending);
                e->pending = NULL;
            } else {
                if (stats) stats->done_integrated++;
                if (merge_loaded_block_entities(w, &done->block_entities) != 0) {
                    log_error("failed to merge block entities for chunk key=%" PRId64, done->key);
                }
            }
        } else {
            if (stats) stats->done_discarded++;
            mc_chunk_destroy(done->chunk);
            free(done->chunk);
        }
        if (e && e->state == CHUNK_SLOT_READY && e->chunk) reconcile_chunk_neighbors(w, e->chunk);
        mc_be_store_destroy(&done->block_entities);
        free(done);
        done = next;
    }
    if (stats) stats->integrate_us = mc_now_us() - integration_start_us;

    /* 2) Save at a bounded rate so large worlds do not monopolize the tick. */
    if (w->save_mode!=MC_SAVE_IMMEDIATE && w->world_path && w->chunk_list_len && !w->save_error) {
        (void)save_group(w,stats);
    } else if (w->save_mode==MC_SAVE_IMMEDIATE && w->world_path && *w->world_path && w->chunk_list_len > 0) {
        size_t scanned = 0;
        size_t attempts = 0;
        while (scanned < w->chunk_list_len && attempts < ANVIL_SAVE_ATTEMPTS_PER_TICK) {
            if (w->save_cursor >= w->chunk_list_len) w->save_cursor = 0;
            mc_chunk_t *c = w->chunk_list[w->save_cursor];
            w->save_cursor = (w->save_cursor + 1) % w->chunk_list_len;
            scanned++;
            if (stats) stats->saves_scanned++;
            if (!c || !c->dirty) continue;

            attempts++;
            if (stats) stats->saves_attempted++;
            int64_t save_start_us = stats ? mc_now_us() : 0;
            int save_rc = save_chunk_to_store(w, c);
            if (stats) {
                int64_t save_elapsed_us = mc_now_us() - save_start_us;
                stats->save_us += save_elapsed_us;
                if (save_elapsed_us > stats->save_max_us) stats->save_max_us = save_elapsed_us;
            }
            if (save_rc == 0) {
                c->dirty = false;
                if (stats) stats->saves_succeeded++;
                if (c->evict_after_save) {
                    int64_t ck = chunk_key(c->cx, c->cz);
                    chunk_map_remove_key(w, ck);
                    if (stats) stats->evict_after_save_removed++;
                }
            } else if (stats) {
                stats->saves_failed++;
            }
        }
    }

    /* 3) Export debug/perf counters after all integration and save work. */
    if (stats) {
        stats->chunk_count = w->chunk_list_len;
        stats->updates_len = w->updates_len;
        pthread_mutex_lock(&w->job_lock);
        stats->jobs_pending = w->jobs_len;
        pthread_mutex_unlock(&w->job_lock);
        for (size_t i = 0; i < w->chunk_list_len; i++) {
            mc_chunk_t *c = w->chunk_list[i];
            if (c && c->dirty) stats->dirty_chunks++;
        }
    }
}

static mc_furnace_machine_t furnace_machine_for_block_entity_type(mc_block_entity_type_t type) {
    switch (type) {
        case MC_BLOCK_ENTITY_FURNACE: return MC_FURNACE_MACHINE_FURNACE;
        case MC_BLOCK_ENTITY_SMOKER: return MC_FURNACE_MACHINE_SMOKER;
        case MC_BLOCK_ENTITY_BLAST_FURNACE: return MC_FURNACE_MACHINE_BLAST_FURNACE;
        default: return MC_FURNACE_MACHINE_NONE;
    }
}

static mc_container_kind_t furnace_container_kind_for_machine(mc_furnace_machine_t machine) {
    switch (machine) {
        case MC_FURNACE_MACHINE_FURNACE: return MC_CONTAINER_KIND_FURNACE;
        case MC_FURNACE_MACHINE_SMOKER: return MC_CONTAINER_KIND_SMOKER;
        case MC_FURNACE_MACHINE_BLAST_FURNACE: return MC_CONTAINER_KIND_BLAST_FURNACE;
        default: return MC_CONTAINER_KIND_NONE;
    }
}

static bool world_state_key_is_prefix(int32_t state_id, const char *prefix) {
    const char *key = mc_block_state_key(state_id);
    return key && prefix && strncmp(key, prefix, strlen(prefix)) == 0;
}

static bool world_state_is_furnace_like(int32_t state_id) {
    return world_state_key_is_prefix(state_id, "minecraft:furnace[") ||
           world_state_key_is_prefix(state_id, "minecraft:smoker[") ||
           world_state_key_is_prefix(state_id, "minecraft:blast_furnace[");
}

static int32_t world_furnace_lit_state_id(int32_t state_id, bool lit) {
    if (!world_state_is_furnace_like(state_id)) return state_id;
    const char *key = mc_block_state_key(state_id);
    if (!key) return state_id;
    const char *lit_pos = strstr(key, "lit=");
    if (!lit_pos) return state_id;
    const char *after_lit = lit_pos + 4;
    while ((*after_lit >= 'a' && *after_lit <= 'z') || (*after_lit >= 'A' && *after_lit <= 'Z')) after_lit++;

    char next_key[192];
    size_t prefix_len = (size_t)(lit_pos - key);
    int n = snprintf(next_key, sizeof(next_key), "%.*slit=%s%s", (int)prefix_len, key, lit ? "true" : "false", after_lit);
    if (n <= 0 || (size_t)n >= sizeof(next_key)) return state_id;
    return mc_world_runtime_state_id_from_key(next_key, state_id);
}

int mc_world_tick_furnaces(mc_world_t *w, mc_world_container_open_fn is_open, void *ctx) {
    return mc_world_tick_furnaces_profiled(w, is_open, ctx, NULL);
}

int mc_world_tick_furnaces_profiled(mc_world_t *w, mc_world_container_open_fn is_open, void *ctx,
                                    mc_world_furnace_tick_stats_t *stats) {
    if (stats) memset(stats, 0, sizeof(*stats));
    if (!w) return 0;
    bool perf = mc_perf_enabled();
    int64_t perf_start_us = perf ? mc_now_us() : 0;
    size_t scanned_entities = 0;
    size_t machine_entities = 0;
    size_t open_skipped = 0;
    size_t ticked = 0;
    size_t changed = 0;
    size_t lit_state_changes = 0;
    size_t errors = 0;
    int rc = 0;
    for (size_t i = 0; i < w->block_entities.cap; i++) {
        scanned_entities++;
        if (!w->block_entities.states || w->block_entities.states[i] != 1) continue;
        mc_block_entity_t *entity = &w->block_entities.entities[i];
        mc_furnace_machine_t machine = furnace_machine_for_block_entity_type(entity->type);
        if (machine == MC_FURNACE_MACHINE_NONE) continue;
        machine_entities++;

        mc_pos_t pos = w->block_entities.positions[i];
        mc_container_kind_t kind = furnace_container_kind_for_machine(machine);
        if (is_open && is_open(ctx, kind, pos.x, pos.y, pos.z)) {
            open_skipped++;
            continue;
        }

        int32_t state_id = -1;
        if (mc_world_get_block(w, pos.x, pos.y, pos.z, &state_id) != 0 || !world_state_is_furnace_like(state_id)) continue;
        ticked++;
        int tick_rc = mc_furnace_tick_block_entity(entity, machine);
        if (tick_rc < 0) {
            rc = -1;
            errors++;
            continue;
        }
        bool is_burning = entity->data.container.furnace_burn_time > 0;
        bool state_changed = false;
        int32_t next_state = world_furnace_lit_state_id(state_id, is_burning);
        if (next_state >= 0 && next_state != state_id) {
            if (mc_world_set_block(w, pos.x, pos.y, pos.z, next_state) != 0) {
                rc = -1;
                errors++;
            } else {
                state_changed = true;
                lit_state_changes++;
            }
        }
        if (tick_rc > 0 || state_changed) changed++;
        if ((tick_rc > 0 || state_changed) && mc_world_mark_chunk_dirty_at(w, pos.x, pos.z) != 0) {
            rc = -1;
            errors++;
        }
    }
    if (stats) {
        stats->scanned_entities = scanned_entities;
        stats->machine_entities = machine_entities;
        stats->open_skipped = open_skipped;
        stats->ticked = ticked;
        stats->changed = changed;
        stats->lit_state_changes = lit_state_changes;
        stats->errors = errors;
    }
    if (perf) {
        int64_t elapsed_us = mc_now_us() - perf_start_us;
        if (elapsed_us >= mc_perf_slow_us()) {
            log_info("perf furnaces: elapsed=%.3fms scanned=%zu machines=%zu open_skipped=%zu ticked=%zu changed=%zu lit_state_changes=%zu errors=%zu",
                     (double)elapsed_us / 1000.0,
                     scanned_entities,
                     machine_entities,
                     open_skipped,
                     ticked,
                     changed,
                     lit_state_changes,
                     errors);
        }
    }
    return rc;
}

static int cmp_i64(const void *a, const void *b) {
    int64_t aa = *(const int64_t *)a;
    int64_t bb = *(const int64_t *)b;
    return (aa > bb) - (aa < bb);
}

static bool i64_sorted_contains(const int64_t *sorted, size_t len, int64_t key) {
    if (!sorted || len == 0) return false;
    size_t lo = 0;
    size_t hi = len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int64_t v = sorted[mid];
        if (v == key) return true;
        if (v < key) lo = mid + 1;
        else hi = mid;
    }
    return false;
}

size_t mc_world_evict_outside(mc_world_t *w, const int64_t *keep_keys, size_t keep_len, size_t budget) {
    if (!w) return 0;

    int64_t *sorted = NULL;
    if (keep_len > 0) {
        sorted = (int64_t *)malloc(keep_len * sizeof(*sorted));
        if (!sorted) {
            log_error("mc_world_evict_outside: OOM building keep-set (len=%zu)", keep_len);
            return 0;
        }
        memcpy(sorted, keep_keys, keep_len * sizeof(*sorted));
        qsort(sorted, keep_len, sizeof(*sorted), cmp_i64);
    }

    size_t evicted = 0;
    size_t i = 0;
    while (i < w->chunk_list_len) {
        mc_chunk_t *c = w->chunk_list[i];
        if (!c) {
            i++;
            continue;
        }
        int64_t key = chunk_key(c->cx, c->cz);
        bool keep = i64_sorted_contains(sorted, keep_len, key);
        if (keep) {
            c->evict_after_save = false;
            i++;
            continue;
        }
        if (c->dirty || c->save_pending) {
            c->evict_after_save = true;
            i++;
            continue;
        }
        if (evicted >= budget) {
            i++;
            continue;
        }

        chunk_map_remove_key(w, key);
        evicted++;
    }

    free(sorted);
    return evicted;
}

const mc_block_update_t *mc_world_updates(const mc_world_t *w, size_t *out_len) {
    if (out_len) *out_len = w ? w->updates_len : 0;
    return w ? w->updates : NULL;
}

void mc_world_clear_updates(mc_world_t *w) {
    if (!w) return;
    w->updates_len = 0;
}
