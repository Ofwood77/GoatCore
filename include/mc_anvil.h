#ifndef MC_ANVIL_H
#define MC_ANVIL_H

#include <stddef.h>
#include <stdint.h>

/* Reads and inflates one chunk's raw NBT payload from a region file (r.X.Z.mca).
 * local_x/local_z are in [0..31]. Output is malloc'ed and must be freed by caller.
 * Returns:
 *   0 on success
 *   1 if the chunk is not present in the region file
 *  -1 on error */
int mc_anvil_read_chunk_nbt(const char *region_path, int local_x, int local_z, uint8_t **out, size_t *out_len);

/* Writes one chunk's raw NBT payload into a region file (r.X.Z.mca), using zlib compression (type=0x02).
 * local_x/local_z are in [0..31]. Returns 0 on success, -1 on error. */
int mc_anvil_write_chunk_nbt(const char *region_path, int local_x, int local_z, const uint8_t *nbt, size_t nbt_len);

#endif /* MC_ANVIL_H */
