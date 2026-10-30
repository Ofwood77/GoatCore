#ifndef MC_CHUNK_STORE_H
#define MC_CHUNK_STORE_H

#include "block_entity_store.h"
#include "mc_world.h"
#include "mc_atomic_file.h"

int mc_chunk_store_read(const char *world_path, int32_t cx, int32_t cz, mc_chunk_t *out, mc_block_entity_store_t *be_store);
/* Synchronous: caller owns the chunk/BE snapshot, normally the authoritative
 * tick or a private loading worker. See MC_SAVE_DURABILITY_UNCERTAIN (-2).
 * World initialization establishes durable directories before live writes.
 * Direct users must prepare world/chunks with mc_persistence_mkdirs and retry
 * that preparation after a directory-creation/sync error before writing. */
int mc_chunk_store_write(const char *world_path, const mc_chunk_t *chunk, const mc_block_entity_store_t *be_store);

#endif /* MC_CHUNK_STORE_H */
