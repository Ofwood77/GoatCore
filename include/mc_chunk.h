#ifndef MC_CHUNK_H
#define MC_CHUNK_H

#include "paletted_container.h"

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
    mc_paletted_container_t sections[MC_WORLD_SECTION_COUNT];
    bool loaded;
    bool dirty;
    bool evict_after_save;
    size_t list_index;
    uint64_t revision;
    /* Runtime-only persistence generations; never serialized into Vanilla NBT. */
    uint64_t save_generation;
    uint64_t durable_generation;
    uint64_t inflight_generation;
    bool save_pending;
    bool save_requested;
    /* Native-only immutable last visible NBT, owned with this chunk. */
    uint8_t *storage_nbt;
    size_t storage_nbt_len;
    /* Native opaque-data protection, one bit per block; absent in .bin. */
    uint8_t *storage_guard;
    bool storage_readonly;
    uint64_t network_cache_revision;
    uint8_t *network_chunkdata;
    size_t network_chunkdata_len;
    uint8_t *network_heightmaps;
    size_t network_heightmaps_len;
} mc_chunk_t;

int mc_chunk_init(mc_chunk_t *chunk, int32_t cx, int32_t cz, mc_global_state_id_t fill_state);
void mc_chunk_destroy(mc_chunk_t *chunk);
mc_global_state_id_t mc_chunk_get_block(const mc_chunk_t *chunk, int x, int y, int z);
int mc_chunk_set_block(mc_chunk_t *chunk, int x, int y, int z, mc_global_state_id_t global_id);
/* Tick/private owner only. Independent sections/source NBT, no network cache or
 * live pointers. Fails before allocation if the explicit snapshot budget is too
 * small. Caller destroys out exactly once, including on later write failure. */
int mc_chunk_save_snapshot(const mc_chunk_t *source, mc_chunk_t *out, size_t budget, size_t *bytes);
int mc_chunk_mark_dirty(mc_chunk_t *chunk);
/* Tick-owned completion. A pending generation must match. Transfers the visible
 * native source from snapshot only on 0/-2; only 0 advances durable_generation.
 * Newer live mutations remain dirty. A stale completion changes nothing. */
int mc_chunk_complete_save(mc_chunk_t *live, mc_chunk_t *snapshot, int result);

#endif /* MC_CHUNK_H */
