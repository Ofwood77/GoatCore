#include "mc_block_drops.h"

#include "generated_block_loot.h"

static uint64_t mix_u64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static uint32_t block_drop_rng(int32_t state_id, int32_t x, int32_t y, int32_t z, int64_t now_ms) {
    uint64_t seed = (uint64_t)(uint32_t)state_id;
    seed ^= mix_u64((uint64_t)(uint32_t)x << 32 | (uint32_t)z);
    seed ^= mix_u64((uint64_t)(uint32_t)y << 32 | (uint32_t)(now_ms & 0xFFFFFFFFu));
    return (uint32_t)mix_u64(seed);
}

bool mc_block_drop_resolve_default(int32_t state_id, bool allow_default_drop, int32_t x, int32_t y, int32_t z,
                                   int64_t now_ms, mc_block_drop_t *out_drop) {
    if (out_drop) *out_drop = (mc_block_drop_t){-1, 0};
    if (!allow_default_drop) return false;

    const mc_block_loot_entry_t *entry = mc_block_loot_entry_from_state(state_id);
    if (!entry) return false;
    if ((entry->flags & MC_BLOCK_LOOT_FLAG_SUPPORTED) == 0u) return false;
    if (entry->item_id < 0 || entry->count == 0u) return false;

    uint16_t min_count = entry->count;
    uint16_t max_count = entry->count_max;
    if (max_count < min_count) max_count = min_count;

    int32_t count = (int32_t)min_count;
    if (max_count > min_count) {
        uint32_t range = (uint32_t)max_count - (uint32_t)min_count + 1u;
        count += (int32_t)(block_drop_rng(state_id, x, y, z, now_ms) % range);
    }

    if (out_drop) {
        out_drop->item_id = entry->item_id;
        out_drop->count = count;
    }
    return true;
}
