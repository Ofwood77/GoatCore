#ifndef MC_ANVIL_H
#define MC_ANVIL_H

#include "block_entity_store.h"
#include "mc_chunk.h"
#include "mc_nbt.h"
#include "mc_atomic_file.h"
#include "mc_region_storage.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Reads and inflates one chunk's raw NBT payload from a region file (r.X.Z.mca).
 * local_x/local_z are in [0..31]. Output is malloc'ed and must be freed by caller.
 * Returns:
 *   0 on success
 *   1 if the chunk is not present in the region file
 *  -1 on error */
/* Reuses a pinned region index; no repeated header scan. Same output contract. */
int mc_anvil_read_chunk_indexed(mc_region_file_t *file, unsigned slot, uint8_t **out, size_t *out_len);
int mc_anvil_read_chunk_nbt(const char *region_path, int local_x, int local_z, uint8_t **out, size_t *out_len);

/* Loads one Anvil chunk directly into an initialized mc_chunk_t.
 * chunk_x/chunk_z are absolute chunk coordinates; region_path selects the .mca file.
 * Returns:
 *   0 on success
 *   1 if the chunk is not present in the region file
 *  -1 on parse/validation/decode error */
int mc_anvil_load_chunk(const char *region_path,
                        int chunk_x,
                        int chunk_z,
                        mc_chunk_t *out_chunk,
                        mc_block_entity_store_t *be_store,
                        mc_arena_t *temp_arena);

/* Decode one raw chunk NBT payload into an initialized mc_chunk_t.
 * chunk_x/chunk_z are the expected absolute chunk coordinates encoded in xPos/zPos. */
int mc_anvil_decode_chunk_nbt(const uint8_t *nbt_buf,
                              size_t nbt_len,
                              int chunk_x,
                              int chunk_z,
                              mc_chunk_t *out_chunk,
                              mc_block_entity_store_t *be_store,
                              mc_arena_t *temp_arena);

/* Writes one chunk's raw NBT payload into a region file (r.X.Z.mca), using zlib compression (type=0x02).
 * local_x/local_z are in [0..31]. Atomic replacement under a persistent advisory
 * .lock shared by cooperating writers. 0 acknowledges file+directory fsync;
 * -1 means no replacement; MC_SAVE_DURABILITY_UNCERTAIN means replacement is
 * visible but its final acknowledgement failed. Never roll back that result.
 * External .mcc and unsupported compression cause a fail-closed rejection.
 * Caller owns an immutable NBT snapshot and the durable storage directory. */
int mc_anvil_write_chunk_nbt(const char *region_path, int local_x, int local_z, const uint8_t *nbt, size_t nbt_len);
/* Native integration: source comparison under the same region lock as publish.
 * expected==NULL requires an absent slot; mismatches return ESTALE unchanged. */
int mc_anvil_write_chunk_nbt_expected(const char *path, int lx, int lz, const uint8_t *nbt, size_t length,
                                      const uint8_t *expected, size_t expected_length);
/* Strict 4788/padded native path, preserving source NBT outside block edits.
 * Nonempty block entities/protochunks/unknown states are currently refused. */
int mc_anvil_decode_native(const uint8_t *raw, size_t length, int32_t cx, int32_t cz, mc_chunk_t *out);
int mc_anvil_encode_native(const mc_chunk_t *chunk, const mc_block_entity_store_t *entities,
                            uint8_t **out, size_t *length);
int mc_anvil_write_chunk(const char *region_path, const mc_chunk_t *chunk, const mc_block_entity_store_t *be_store);
int mc_anvil_encode_chunk_nbt(const mc_chunk_t *chunk, const mc_block_entity_store_t *be_store, uint8_t **out_raw, size_t *out_raw_len);

#endif /* MC_ANVIL_H */
