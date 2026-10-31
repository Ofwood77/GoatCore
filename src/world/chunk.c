#include "mc_chunk.h"
#include "mc_util.h"
#include "mc_atomic_file.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

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
    free(chunk->storage_nbt);
    free(chunk->storage_guard);
    free(chunk->network_chunkdata);
    free(chunk->network_heightmaps);
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
    mc_global_state_id_t old_id = mc_paletted_container_get_block(&chunk->sections[sec_index], x, local_y, z);
    if (old_id != global_id && (chunk->revision == UINT64_MAX || chunk->save_generation == UINT64_MAX)) {
        errno = EOVERFLOW;
        return -1;
    }
    int rc = mc_paletted_container_set_block(&chunk->sections[sec_index], x, local_y, z, global_id);
    if (rc == 0 && old_id != global_id) {
        size_t invalidated = chunk->network_chunkdata_len + chunk->network_heightmaps_len;
        mc_save_clock_t span = invalidated ? mc_save_profile_begin() : (mc_save_clock_t){0};
        chunk->revision++;
        chunk->save_generation++;
        free(chunk->network_chunkdata);
        chunk->network_chunkdata = NULL;
        chunk->network_chunkdata_len = 0;
        free(chunk->network_heightmaps);
        chunk->network_heightmaps = NULL;
        chunk->network_heightmaps_len = 0;
        mc_save_profile_end("cache_invalidate", chunk->cx, chunk->cz, span, invalidated, 0);
    }
    return rc;
}

int mc_chunk_mark_dirty(mc_chunk_t *chunk) {
    if (!chunk || chunk->save_generation == UINT64_MAX) {
        errno = chunk ? EOVERFLOW : EINVAL;
        return -1;
    }
    chunk->save_generation++;
    chunk->dirty = true;
    return 0;
}

int mc_chunk_complete_save(mc_chunk_t *live, mc_chunk_t *snapshot, int result) {
    if (!live || !snapshot || !live->save_pending || live->cx != snapshot->cx || live->cz != snapshot->cz ||
        live->inflight_generation != snapshot->save_generation ||
        live->save_generation < snapshot->save_generation ||
        live->durable_generation > snapshot->save_generation) {
        errno = ESTALE;
        return -1;
    }
    if (result == 0 || result == MC_SAVE_DURABILITY_UNCERTAIN) {
        free(live->storage_nbt);
        live->storage_nbt = snapshot->storage_nbt;
        live->storage_nbt_len = snapshot->storage_nbt_len;
        snapshot->storage_nbt = NULL;
        snapshot->storage_nbt_len = 0;
    }
    if (result == 0)
        live->durable_generation = snapshot->save_generation;
    live->dirty = result != 0 || live->save_generation != snapshot->save_generation;
    if (!live->dirty)
        live->save_requested = false;
    live->save_pending = false;
    return 0;
}

int mc_chunk_save_snapshot(const mc_chunk_t *source, mc_chunk_t *out, size_t budget, size_t *bytes) {
    if (!source || !out || source == out || !bytes) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof(*out));
    size_t required = sizeof(*out);
    size_t sizes[MC_WORLD_SECTION_COUNT + 2];
    for (unsigned i = 0; i < MC_WORLD_SECTION_COUNT; i++) {
        if (source->sections[i].word_count > SIZE_MAX / sizeof(uint64_t)) {
            errno = EOVERFLOW;
            return -1;
        }
        sizes[i] = source->sections[i].word_count * sizeof(uint64_t);
    }
    sizes[MC_WORLD_SECTION_COUNT] = source->storage_nbt_len;
    sizes[MC_WORLD_SECTION_COUNT + 1] = source->storage_guard ? MC_BLOCKS_PER_CHUNK / 8 : 0;
    for (unsigned i = 0; i < MC_WORLD_SECTION_COUNT + 2; i++) {
        if (sizes[i] > SIZE_MAX - required) {
            errno = EOVERFLOW;
            return -1;
        }
        required += sizes[i];
    }
    *bytes = required;
    if (required > budget) {
        errno = EFBIG;
        return -1;
    }
    out->cx = source->cx;
    out->cz = source->cz;
    out->revision = source->revision;
    out->save_generation = source->save_generation;
    out->durable_generation = source->durable_generation;
    out->loaded = source->loaded;
    out->dirty = source->dirty;
    out->storage_readonly = source->storage_readonly;
    for (unsigned i = 0; i < MC_WORLD_SECTION_COUNT; i++) {
        out->sections[i] = source->sections[i];
        out->sections[i].words = NULL;
        if (sizes[i]) {
            out->sections[i].words = malloc(sizes[i]);
            if (!out->sections[i].words)
                goto bad;
            memcpy(out->sections[i].words, source->sections[i].words, sizes[i]);
        }
    }
    if (source->storage_nbt_len) {
        out->storage_nbt = malloc(source->storage_nbt_len);
        if (!out->storage_nbt)
            goto bad;
        memcpy(out->storage_nbt, source->storage_nbt, source->storage_nbt_len);
        out->storage_nbt_len = source->storage_nbt_len;
    }
    if (source->storage_guard) {
        out->storage_guard = malloc(MC_BLOCKS_PER_CHUNK / 8);
        if (!out->storage_guard)
            goto bad;
        memcpy(out->storage_guard, source->storage_guard, MC_BLOCKS_PER_CHUNK / 8);
    }
    return 0;
bad:;
    int saved = errno;
    mc_chunk_destroy(out);
    errno = saved;
    return -1;
}
