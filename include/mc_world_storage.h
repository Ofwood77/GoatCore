#ifndef MC_WORLD_STORAGE_H
#define MC_WORLD_STORAGE_H

#include "mc_chunk.h"
#include "block_entity_store.h"

typedef enum { MC_STORAGE_BIN = 0, MC_STORAGE_ANVIL_ATOMIC = 1 } mc_storage_kind_t;
typedef struct mc_world_storage mc_world_storage_t;
const char *mc_storage_name(mc_storage_kind_t kind);
int mc_storage_parse(const char *name, mc_storage_kind_t *kind);
/* Immutable backend selection. No fallback, migration or incremental writer.
 * Native sessions own world/session locks until all workers and saves finish.
 * path is borrowed and must outlive the store. */
int mc_storage_open(const char *path, mc_storage_kind_t kind, mc_world_storage_t **out);
bool mc_storage_is_native(const mc_world_storage_t *store);
void mc_storage_close(mc_world_storage_t *store);
/* out must own no previous allocations (a fresh uniform chunk is allowed). */
int mc_storage_read(const mc_world_storage_t *store, int32_t cx, int32_t cz, mc_chunk_t *out,
                    mc_block_entity_store_t *entities);
/* Caller exclusively owns the live/private chunk. Only 0 clears dirty; -2
 * retains dirty but advances the visible source snapshot for safe retry. */
int mc_storage_write(const mc_world_storage_t *store, mc_chunk_t *chunk,
                     const mc_block_entity_store_t *entities);
/* Native only. All chunks belong to ONE region, owned exclusively by caller.
 * Serializes each once and atomically replaces the region once. On 0/-2 all
 * private source buffers advance; no live-world pointer is retained. */
int mc_storage_write_region(const mc_world_storage_t *store, mc_chunk_t *const *chunks, size_t count);
#endif
