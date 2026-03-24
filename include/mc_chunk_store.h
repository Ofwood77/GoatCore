#ifndef MC_CHUNK_STORE_H
#define MC_CHUNK_STORE_H

#include "mc_world.h"

int mc_chunk_store_read(const char *world_path, int32_t cx, int32_t cz, mc_chunk_t *out);
int mc_chunk_store_write(const char *world_path, const mc_chunk_t *chunk);

#endif /* MC_CHUNK_STORE_H */
