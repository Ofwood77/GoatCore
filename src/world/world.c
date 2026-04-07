#include "mc_world.h"

#include "generated_registries.h"
#include "mc_chunk_store.h"
#include "mc_anvil.h"
#include "mc_util.h"
#include "stb_perlin.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ANVIL_SAVE_ATTEMPTS_PER_TICK 1
#define MAX_WORKERS 4

typedef struct {
    int32_t x;
    int32_t y;
    int32_t z;
    int32_t state_id;
} mc_pending_mod_t;

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

typedef struct {
    mc_chunk_entry_t *entries;
    size_t cap;
    size_t len;
    size_t tombs;
} mc_chunk_map_t;

typedef struct {
    int32_t cx;
    int32_t cz;
    uint32_t priority;
} mc_chunk_job_t;

typedef struct mc_chunk_done {
    int64_t key;
    mc_chunk_t *chunk;
    bool generated;
    struct mc_chunk_done *next;
} mc_chunk_done_t;

struct mc_world {
    char *world_path;
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
    return ((int64_t)cx << 32) | (uint32_t)cz;
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
    return mc_be_store_put(&w->block_entities, (mc_pos_t){x, y, z}, *entity) ? 0 : -1;
}

int mc_world_remove_block_entity(mc_world_t *w, int32_t x, int32_t y, int32_t z) {
    if (!w) return -1;
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

static int mkdir_p(const char *path, mode_t mode) {
    if (!path || !*path) return -1;

    char tmp[1024];
    size_t n = strlen(path);
    if (n >= sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(tmp, path, n + 1);

    while (n > 1 && tmp[n - 1] == '/') {
        tmp[n - 1] = '\0';
        n--;
    }

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        struct stat st;
        if (stat(tmp, &st) != 0) {
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
                *p = '/';
                return -1;
            }
        } else if (!S_ISDIR(st.st_mode)) {
            *p = '/';
            errno = ENOTDIR;
            return -1;
        }
        *p = '/';
    }

    struct stat st;
    if (stat(tmp, &st) != 0) {
        if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
    } else if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

static int ensure_world_dirs(const char *world_path) {
    if (!world_path || !*world_path) return -1;
    if (mkdir_p(world_path, 0755) != 0) return -1;

    char region[1024];
    int n = snprintf(region, sizeof(region), "%s/region", world_path);
    if (n <= 0 || (size_t)n >= sizeof(region)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (mkdir_p(region, 0755) != 0) return -1;

    char chunks[1024];
    n = snprintf(chunks, sizeof(chunks), "%s/chunks", world_path);
    if (n <= 0 || (size_t)n >= sizeof(chunks)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (mkdir_p(chunks, 0755) != 0) return -1;

    char players[1024];
    n = snprintf(players, sizeof(players), "%s/players", world_path);
    if (n <= 0 || (size_t)n >= sizeof(players)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (mkdir_p(players, 0755) != 0) return -1;

    char containers[1024];
    n = snprintf(containers, sizeof(containers), "%s/containers", world_path);
    if (n <= 0 || (size_t)n >= sizeof(containers)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (mkdir_p(containers, 0755) != 0) return -1;

    if (access(world_path, R_OK | W_OK | X_OK) != 0) return -1;
    if (access(region, R_OK | W_OK | X_OK) != 0) return -1;
    if (access(chunks, R_OK | W_OK | X_OK) != 0) return -1;
    if (access(players, R_OK | W_OK | X_OK) != 0) return -1;
    if (access(containers, R_OK | W_OK | X_OK) != 0) return -1;
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

static mc_chunk_done_t *done_queue_drain(mc_world_t *w) {
    if (!w) return NULL;
    pthread_mutex_lock(&w->done_lock);
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

static float noise2_seed(int32_t wx, int32_t wz, float freq, int seed) {
    return stb_perlin_noise3_seed((float)wx * freq, 0.0f, (float)wz * freq, 0, 0, 0, seed);
}

static float noise3_seed(int32_t wx, int32_t wy, int32_t wz, float freq, int seed) {
    return stb_perlin_noise3_seed((float)wx * freq, (float)wy * freq, (float)wz * freq, 0, 0, 0, seed);
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

static int save_chunk_to_store(const mc_world_t *w, const mc_chunk_t *chunk) {
    if (!w || !chunk) return -1;
    if (!w->world_path || !*w->world_path) return -1;

    int rc = mc_chunk_store_write(w->world_path, chunk, &w->block_entities);
    if (rc != 0) {
        int e = errno;
        log_error("chunk store save failed chunk=(%d,%d) world=%s (errno=%d %s)", chunk->cx, chunk->cz,
                  w->world_path ? w->world_path : "(null)", e, strerror(e));
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

static void apply_pending_mods(mc_world_t *w, mc_chunk_t *chunk, mc_pending_mods_t *pending) {
    if (!w || !chunk || !pending) return;
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

        bool generated = false;
        bool normalized_on_load = false;

        int store_rc = mc_chunk_store_read(w->world_path, job.cx, job.cz, chunk, &w->block_entities);
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
                log_error("anvil load failed for chunk (%d,%d) world=%s", job.cx, job.cz, w->world_path ? w->world_path : "(null)");
                if (debug_target_chunk(w, job.cx, job.cz)) {
                    log_info("chunk reload debug: worker chunk=(%d,%d) source=generate reason=load_error", job.cx, job.cz);
                }
                mc_world_generate_chunk(w, chunk);
                chunk->dirty = (w->world_path && *w->world_path);
                generated = true;
            }
        }
        if (normalize_chunk_container_states(chunk)) {
            chunk->dirty = true;
            normalized_on_load = true;
        }
        if (normalized_on_load && w->world_path && *w->world_path) {
            if (save_chunk_to_store(w, chunk) == 0) {
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
            continue;
        }
        d->key = chunk_key(job.cx, job.cz);
        d->chunk = chunk;
        d->generated = generated;
        done_queue_push(w, d);
    }
    return NULL;
}

mc_world_t *mc_world_create(const char *world_path, int64_t level_seed) {
    mc_world_t *w = (mc_world_t *)calloc(1, sizeof(*w));
    if (!w) return NULL;

    if (world_path && *world_path) {
        w->world_path = strdup(world_path);
        if (!w->world_path) {
            free(w);
            return NULL;
        }
        if (ensure_world_dirs(w->world_path) != 0) {
            int e = errno;
            log_error("world_path not writable: %s (errno=%d %s)", w->world_path, e, strerror(e));
            free(w->world_path);
            free(w);
            return NULL;
        }
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
        free(w->world_path);
        free(w);
        return NULL;
    }

    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int wc = (cpus > 1) ? (int)(cpus - 1) : 1;
    if (wc < 1) wc = 1;
    if (wc > MAX_WORKERS) wc = MAX_WORKERS;
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

void mc_world_destroy(mc_world_t *w) {
    if (!w) return;

    pthread_mutex_lock(&w->job_lock);
    w->jobs_stop = true;
    pthread_cond_broadcast(&w->job_cv);
    pthread_mutex_unlock(&w->job_lock);

    for (int i = 0; i < w->worker_count; i++) {
        if (w->workers[i]) pthread_join(w->workers[i], NULL);
    }

    mc_chunk_done_t *done = done_queue_drain(w);
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
                apply_pending_mods(w, done->chunk, e->pending);
                pending_mods_free(e->pending);
                e->pending = NULL;
            }
        } else {
            mc_chunk_destroy(done->chunk);
            free(done->chunk);
        }
        free(done);
        done = next;
    }

    if (w->world_path && *w->world_path) {
        for (size_t i = 0; i < w->chunk_list_len; i++) {
            mc_chunk_t *c = w->chunk_list[i];
            if (!c || !c->dirty) continue;
            if (save_chunk_to_store(w, c) == 0) c->dirty = false;
        }
    }

    chunk_map_free(&w->chunks);
    free(w->chunk_list);
    free(w->updates);
    free(w->jobs);
    mc_be_store_destroy(&w->block_entities);
    free(w->world_path);

    pthread_mutex_destroy(&w->job_lock);
    pthread_cond_destroy(&w->job_cv);
    pthread_mutex_destroy(&w->done_lock);

    free(w);
}

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

int mc_world_set_block(mc_world_t *w, int32_t x, int32_t y, int32_t z, int32_t state_id) {
    if (!w) return -1;
    if (y < MC_WORLD_MIN_Y || y >= MC_WORLD_MIN_Y + MC_WORLD_HEIGHT) return -1;
    state_id = mc_world_normalize_container_state_id(state_id);

    int32_t cx = 0, cz = 0;
    int lx = 0, lz = 0;
    if (coords_to_chunk(x, z, &cx, &cz, &lx, &lz) != 0) return -1;

    int64_t key = chunk_key(cx, cz);
    mc_chunk_entry_t *e = chunk_map_get(&w->chunks, key);
    if (e && e->state == CHUNK_SLOT_READY && e->chunk) {
        mc_chunk_t *chunk = e->chunk;
        int32_t old = (int32_t)mc_chunk_get_block(chunk, lx, y, lz);
        if (old == state_id) return 0;
        if (mc_chunk_set_block(chunk, lx, y, lz, (mc_global_state_id_t)state_id) != 0) return -1;
        chunk->dirty = true;
        if (mc_world_debug_container_match(w, x, y, z) &&
            (mc_world_normalize_container_state_id(old) != old || mc_world_normalize_container_state_id(state_id) == state_id)) {
            log_info("containers debug: set_block chunk=(%d,%d) pos=(%d,%d,%d) requested=%d normalized=%d old=%d old_key=%s new_key=%s",
                     cx, cz, x, y, z, state_id, mc_world_normalize_container_state_id(state_id), old,
                     mc_block_state_key(old) ? mc_block_state_key(old) : "(null)",
                     mc_block_state_key(state_id) ? mc_block_state_key(state_id) : "(null)");
        }
        return updates_push(w, (mc_block_update_t){x, y, z, state_id});
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
    return pending_mods_push(e->pending, (mc_pending_mod_t){x, y, z, state_id});
}

int mc_world_flush_block(mc_world_t *w, int32_t x, int32_t y, int32_t z) {
    if (!w) return -1;
    if (!w->world_path || !*w->world_path) return 0;
    if (y < MC_WORLD_MIN_Y || y >= MC_WORLD_MIN_Y + MC_WORLD_HEIGHT) return -1;

    int32_t cx = 0, cz = 0;
    int lx = 0, lz = 0;
    if (coords_to_chunk(x, z, &cx, &cz, &lx, &lz) != 0) return -1;
    (void)lx;
    (void)lz;

    mc_chunk_t *chunk = mc_world_get_chunk(w, cx, cz, UINT32_MAX);
    if (!chunk) return -1;
    if (!chunk->dirty) return 0;
    if (save_chunk_to_store(w, chunk) != 0) return -1;
    chunk->dirty = false;
    if (mc_world_debug_container_match(w, x, y, z)) {
        int32_t sid = (int32_t)mc_chunk_get_block(chunk, lx, y, lz);
        log_info("containers debug: flush chunk=(%d,%d) block=(%d,%d,%d) state_id=%d key=%s", cx, cz, x, y, z, sid,
                 mc_block_state_key(sid) ? mc_block_state_key(sid) : "(null)");
    }
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
    chunk->dirty = true;
    return 0;
}

void mc_world_tick(mc_world_t *w, int64_t now_ms) {
    (void)now_ms;
    if (!w) return;
    w->tick_count++;

    mc_chunk_done_t *done = done_queue_drain(w);
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
                apply_pending_mods(w, done->chunk, e->pending);
                pending_mods_free(e->pending);
                e->pending = NULL;
            }
        } else {
            mc_chunk_destroy(done->chunk);
            free(done->chunk);
        }
        free(done);
        done = next;
    }

    if (!w->world_path || !*w->world_path) return;
    if (w->chunk_list_len == 0) return;

    size_t scanned = 0;
    size_t attempts = 0;
    while (scanned < w->chunk_list_len && attempts < ANVIL_SAVE_ATTEMPTS_PER_TICK) {
        if (w->save_cursor >= w->chunk_list_len) w->save_cursor = 0;
        mc_chunk_t *c = w->chunk_list[w->save_cursor];
        w->save_cursor = (w->save_cursor + 1) % w->chunk_list_len;
        scanned++;
        if (!c || !c->dirty) continue;

        attempts++;
        if (save_chunk_to_store(w, c) == 0) {
            c->dirty = false;
            if (c->evict_after_save) {
                int64_t ck = chunk_key(c->cx, c->cz);
                chunk_map_remove_key(w, ck);
            }
        }
    }
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
        if (c->dirty) {
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
