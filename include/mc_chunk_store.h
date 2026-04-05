#ifndef MC_CHUNK_STORE_H
#define MC_CHUNK_STORE_H

#include "block_entity_store.h"
#include "mc_world.h"

int mc_chunk_store_read(const char *world_path, int32_t cx, int32_t cz, mc_chunk_t *out, mc_block_entity_store_t *be_store);
int mc_chunk_store_write(const char *world_path, const mc_chunk_t *chunk, const mc_block_entity_store_t *be_store);

#endif /* MC_CHUNK_STORE_H */
