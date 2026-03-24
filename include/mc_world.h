#ifndef MC_WORLD_H
#define MC_WORLD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MC_WORLD_MIN_Y (-64)
#define MC_WORLD_HEIGHT 384
#define MC_WORLD_SECTION_COUNT 24
#define MC_CHUNK_XZ 16

#define MC_BLOCKS_PER_CHUNK (MC_CHUNK_XZ * MC_CHUNK_XZ * MC_WORLD_HEIGHT)

typedef struct {
    int32_t cx;
    int32_t cz;
    int32_t blocks[MC_BLOCKS_PER_CHUNK]; /* index = (y_index*16 + z)*16 + x */
    bool loaded;
    bool dirty;
    bool evict_after_save;
    size_t list_index;
} mc_chunk_t;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t z;
    int32_t state_id;
} mc_block_update_t;

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

mc_world_t *mc_world_create(const char *world_path, int64_t level_seed);
void mc_world_destroy(mc_world_t *w);

const char *mc_world_path(const mc_world_t *w);
const mc_world_ids_t *mc_world_ids(const mc_world_t *w);
int32_t mc_world_normalize_container_state_id(int32_t state_id);
bool mc_world_debug_containers_enabled(const mc_world_t *w);
bool mc_world_debug_container_match(const mc_world_t *w, int32_t x, int32_t y, int32_t z);

void mc_world_set_generation_params(mc_world_t *w, float freq, int32_t base_y, int32_t amp);
void mc_world_generate_chunk(mc_world_t *w, mc_chunk_t *chunk);

/* Returns the chunk if it is ready in memory; otherwise enqueues an async load/gen and returns NULL. */
mc_chunk_t *mc_world_get_chunk(mc_world_t *w, int32_t cx, int32_t cz, uint32_t priority);

int mc_world_get_block(mc_world_t *w, int32_t x, int32_t y, int32_t z, int32_t *out_state_id);
int mc_world_set_block(mc_world_t *w, int32_t x, int32_t y, int32_t z, int32_t state_id);
int mc_world_flush_block(mc_world_t *w, int32_t x, int32_t y, int32_t z);

void mc_world_tick(mc_world_t *w, int64_t now_ms);

/* Marks chunks not in keep-set for eviction, freeing clean chunks immediately and dirty chunks after save. */
size_t mc_world_evict_outside(mc_world_t *w, const int64_t *keep_keys, size_t keep_len, size_t budget);

const mc_block_update_t *mc_world_updates(const mc_world_t *w, size_t *out_len);
void mc_world_clear_updates(mc_world_t *w);

#endif /* MC_WORLD_H */
