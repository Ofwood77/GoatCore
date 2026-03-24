#ifndef MC_PACKED_H
#define MC_PACKED_H

#include <stddef.h>
#include <stdint.h>

/* Packed integer helpers (LSB-first within each 64-bit word), used for Anvil paletted data and heightmaps. */

size_t mc_packed_compact_long_count(size_t value_count, int bits);

/* Allocates `*out_longs` (calloc) and packs values into it. Caller frees `*out_longs`. */
int mc_packed_pack_compact_u32(const uint32_t *values, size_t value_count, int bits, int64_t **out_longs, int32_t *out_len);

/* Unpacks one value; returns 0 if out-of-range. */
uint32_t mc_packed_unpack_compact_u32(const int64_t *longs, int32_t longs_len, size_t index, int bits);

#endif /* MC_PACKED_H */

