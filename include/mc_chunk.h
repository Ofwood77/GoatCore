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
} mc_chunk_t;

int mc_chunk_init(mc_chunk_t *chunk, int32_t cx, int32_t cz, mc_global_state_id_t fill_state);
void mc_chunk_destroy(mc_chunk_t *chunk);
mc_global_state_id_t mc_chunk_get_block(const mc_chunk_t *chunk, int x, int y, int z);
int mc_chunk_set_block(mc_chunk_t *chunk, int x, int y, int z, mc_global_state_id_t global_id);

#endif /* MC_CHUNK_H */
