#include "mc_packed.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

size_t mc_packed_compact_long_count(size_t value_count, int bits) {
    if (bits <= 0) return 0;
    if (value_count == 0) return 0;
    size_t total_bits = value_count * (size_t)bits;
    return (total_bits + 63) / 64;
}

int mc_packed_pack_compact_u32(const uint32_t *values, size_t value_count, int bits, int64_t **out_longs, int32_t *out_len) {
    if (out_longs) *out_longs = NULL;
    if (out_len) *out_len = 0;
    if (!values || !out_longs || !out_len) return -1;
    if (bits <= 0 || bits > 32) return -1;
    if (value_count == 0) return 0;

    size_t long_count = mc_packed_compact_long_count(value_count, bits);
    if (long_count == 0 || long_count > (size_t)INT32_MAX) return -1;

    uint64_t *words = (uint64_t *)calloc(long_count, sizeof(*words));
    if (!words) return -1;

    uint64_t mask = (bits == 64) ? UINT64_MAX : ((1ULL << bits) - 1ULL);
    for (size_t i = 0; i < value_count; i++) {
        uint64_t v = (uint64_t)values[i] & mask;
        size_t bit_index = i * (size_t)bits;
        size_t li = bit_index >> 6;
        int shift = (int)(bit_index & 63);
        if (li >= long_count) break;

        words[li] |= v << shift;
        int spill = shift + bits - 64;
        if (spill > 0 && li + 1 < long_count) {
            words[li + 1] |= v >> (bits - spill);
        }
    }

    int64_t *out = (int64_t *)malloc(long_count * sizeof(*out));
    if (!out) {
        free(words);
        return -1;
    }
    for (size_t i = 0; i < long_count; i++) out[i] = (int64_t)words[i];
    free(words);

    *out_longs = out;
    *out_len = (int32_t)long_count;
    return 0;
}

uint32_t mc_packed_unpack_compact_u32(const int64_t *longs, int32_t longs_len, size_t index, int bits) {
    if (!longs || longs_len <= 0) return 0;
    if (bits <= 0 || bits > 32) return 0;

    size_t bit_index = index * (size_t)bits;
    size_t li = bit_index >> 6;
    int shift = (int)(bit_index & 63);
    if (li >= (size_t)longs_len) return 0;

    uint64_t lo = (uint64_t)longs[li];
    uint64_t v = lo >> shift;

    int spill = shift + bits - 64;
    if (spill > 0 && li + 1 < (size_t)longs_len) {
        uint64_t hi = (uint64_t)longs[li + 1];
        v |= hi << (bits - spill);
    }

    uint64_t mask = (bits == 64) ? UINT64_MAX : ((1ULL << bits) - 1ULL);
    return (uint32_t)(v & mask);
}

