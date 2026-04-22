#ifndef MC_WORLD_H
#define MC_WORLD_H

#include "block_entity_store.h"
#include "mc_chunk.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Per-tick block changes to broadcast after the world has been mutated. */
typedef struct {
    int32_t x;
    int32_t y;
    int32_t z;
    int32_t state_id;
} mc_block_update_t;

/* Cached runtime IDs for the handful of blocks that world generation and
 * gameplay touch constantly. */
typedef struct {
    int32_t air;
    int32_t stone;
    int32_t dirt;
    int32_t grass_block_snowy_false;
    int32_t water_level[16];
    int32_t lava_level[16];
    int32_t fire_age[16];
    int32_t wire_power[16];
    int32_t redstone_block;
    int32_t lamp_lit[2];
} mc_world_ids_t;

typedef struct mc_world mc_world_t;
typedef bool (*mc_world_container_open_fn)(void *ctx, mc_container_kind_t kind, int32_t x, int32_t y, int32_t z);

/* Optional counters for the main tick loop so server.c can explain where time
 * is spent without teaching the world module about logging policy. */
typedef struct {
    size_t done_seen;
    size_t done_integrated;
    size_t done_discarded;
    size_t done_add_failed;
    size_t saves_scanned;
    size_t saves_attempted;
    size_t saves_succeeded;
    size_t saves_failed;
    size_t evict_after_save_removed;
    size_t chunk_count;
    size_t dirty_chunks;
    size_t updates_len;
    size_t jobs_pending;
} mc_world_tick_stats_t;

typedef struct {
    size_t scanned_entities;
    size_t machine_entities;
    size_t open_skipped;
    size_t ticked;
    size_t changed;
    size_t lit_state_changes;
    size_t errors;
} mc_world_furnace_tick_stats_t;

mc_world_t *mc_world_create(const char *world_path, int64_t level_seed);
void mc_world_destroy(mc_world_t *w);

const char *mc_world_path(const mc_world_t *w);
const mc_world_ids_t *mc_world_ids(const mc_world_t *w);
int32_t mc_world_runtime_state_id_from_key(const char *key, int32_t fallback);
int32_t mc_world_normalize_container_state_id(int32_t state_id);
mc_block_entity_t *mc_world_get_block_entity(mc_world_t *w, int32_t x, int32_t y, int32_t z);
int mc_world_put_block_entity(mc_world_t *w, int32_t x, int32_t y, int32_t z, const mc_block_entity_t *entity);
int mc_world_remove_block_entity(mc_world_t *w, int32_t x, int32_t y, int32_t z);
bool mc_world_debug_containers_enabled(const mc_world_t *w);
bool mc_world_debug_container_match(const mc_world_t *w, int32_t x, int32_t y, int32_t z);

void mc_world_set_generation_params(mc_world_t *w, float freq, int32_t base_y, int32_t amp);
void mc_world_generate_chunk(mc_world_t *w, mc_chunk_t *chunk);

/* Returns the chunk if it is ready in memory; otherwise enqueues an async
 * load/gen and returns NULL. Callers that need a deterministic answer should
 * use mc_world_get_block_ready instead of treating NULL as air. */
mc_chunk_t *mc_world_get_chunk(mc_world_t *w, int32_t cx, int32_t cz, uint32_t priority);

int mc_world_get_block(mc_world_t *w, int32_t x, int32_t y, int32_t z, int32_t *out_state_id);
/* Returns 0 with a state when the owning chunk is ready, 1 when it is
 * queued/loading, and -1 on error. This is the strict read used by mining and
 * other protocol flows that must not guess when the world is still loading. */
int mc_world_get_block_ready(mc_world_t *w, int32_t x, int32_t y, int32_t z, int32_t *out_state_id);
int mc_world_set_block(mc_world_t *w, int32_t x, int32_t y, int32_t z, int32_t state_id);
int mc_world_mark_chunk_dirty_at(mc_world_t *w, int32_t x, int32_t z);
int mc_world_flush_block(mc_world_t *w, int32_t x, int32_t y, int32_t z);

void mc_world_tick(mc_world_t *w, int64_t now_ms);
void mc_world_tick_profiled(mc_world_t *w, int64_t now_ms, mc_world_tick_stats_t *stats);
int mc_world_tick_furnaces(mc_world_t *w, mc_world_container_open_fn is_open, void *ctx);
int mc_world_tick_furnaces_profiled(mc_world_t *w, mc_world_container_open_fn is_open, void *ctx,
                                    mc_world_furnace_tick_stats_t *stats);

/* Marks chunks not in keep-set for eviction, freeing clean chunks immediately and dirty chunks after save. */
size_t mc_world_evict_outside(mc_world_t *w, const int64_t *keep_keys, size_t keep_len, size_t budget);

const mc_block_update_t *mc_world_updates(const mc_world_t *w, size_t *out_len);
void mc_world_clear_updates(mc_world_t *w);

#endif /* MC_WORLD_H */
