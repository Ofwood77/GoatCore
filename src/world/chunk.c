#include "mc_chunk.h"

#include <string.h>

static int chunk_section_index_from_y(int y) {
    if (y < MC_WORLD_MIN_Y || y >= MC_WORLD_MIN_Y + MC_WORLD_HEIGHT) return -1;
    return (y - MC_WORLD_MIN_Y) >> 4;
}

static int chunk_local_y_from_world_y(int y) {
    return (y - MC_WORLD_MIN_Y) & 15;
}

int mc_chunk_init(mc_chunk_t *chunk, int32_t cx, int32_t cz, mc_global_state_id_t fill_state) {
    int i;

    if (!chunk) return -1;
    memset(chunk, 0, sizeof(*chunk));
    chunk->cx = cx;
    chunk->cz = cz;
    for (i = 0; i < MC_WORLD_SECTION_COUNT; i++) {
        if (mc_paletted_container_init(&chunk->sections[i], fill_state) != 0) {
            while (--i >= 0) mc_paletted_container_destroy(&chunk->sections[i]);
            memset(chunk, 0, sizeof(*chunk));
            return -1;
        }
    }
    return 0;
}

void mc_chunk_destroy(mc_chunk_t *chunk) {
    int i;

    if (!chunk) return;
    for (i = 0; i < MC_WORLD_SECTION_COUNT; i++) {
        mc_paletted_container_destroy(&chunk->sections[i]);
    }
    memset(chunk, 0, sizeof(*chunk));
}

mc_global_state_id_t mc_chunk_get_block(const mc_chunk_t *chunk, int x, int y, int z) {
    int sec_index;
    int local_y;

    if (!chunk) return 0;
    if (x < 0 || x >= MC_CHUNK_XZ || z < 0 || z >= MC_CHUNK_XZ) return 0;
    sec_index = chunk_section_index_from_y(y);
    if (sec_index < 0) return 0;
    local_y = chunk_local_y_from_world_y(y);
    return mc_paletted_container_get_block(&chunk->sections[sec_index], x, local_y, z);
}

int mc_chunk_set_block(mc_chunk_t *chunk, int x, int y, int z, mc_global_state_id_t global_id) {
    int sec_index;
    int local_y;

    if (!chunk) return -1;
    if (x < 0 || x >= MC_CHUNK_XZ || z < 0 || z >= MC_CHUNK_XZ) return -1;
    sec_index = chunk_section_index_from_y(y);
    if (sec_index < 0) return -1;
    local_y = chunk_local_y_from_world_y(y);
    return mc_paletted_container_set_block(&chunk->sections[sec_index], x, local_y, z, global_id);
}
