#include "block_registry.h"

#include <string.h>

typedef struct {
    const char *key;
    mc_global_state_id_t id;
} mc_state_key_entry_t;

const size_t GLOBAL_BLOCK_COUNT = 4u;
const size_t GLOBAL_BLOCK_STATES_COUNT = 7u;

const mc_block_desc_t GLOBAL_BLOCKS[4] = {
    [0] = {"minecraft:air", 0u, 0u, 0u},
    [1] = {"minecraft:chest", 3u, 3u, 6u},
    [2] = {"minecraft:glass", 2u, 2u, 2u},
    [3] = {"minecraft:stone", 1u, 1u, 1u},
};

const mc_block_properties_t GLOBAL_BLOCK_STATES[7] = {
    [0] = {MC_BLOCK_FLAG_VALID | MC_BLOCK_FLAG_IS_DEFAULT_STATE | MC_BLOCK_FLAG_IS_AIR, 0u, 0u, 0u},
    [1] = {MC_BLOCK_FLAG_VALID | MC_BLOCK_FLAG_IS_DEFAULT_STATE, 0u, 0u, 3u},
    [2] = {MC_BLOCK_FLAG_VALID | MC_BLOCK_FLAG_IS_DEFAULT_STATE, 0u, 0u, 2u},
    [3] = {MC_BLOCK_FLAG_VALID | MC_BLOCK_FLAG_IS_DEFAULT_STATE, 0u, 0u, 1u},
    [4] = {MC_BLOCK_FLAG_VALID, 0u, 0u, 1u},
    [5] = {MC_BLOCK_FLAG_VALID, 0u, 0u, 1u},
    [6] = {MC_BLOCK_FLAG_VALID, 0u, 0u, 1u},
};

static const char *mc_state_keys_by_id[7] = {
    [0] = "minecraft:air",
    [1] = "minecraft:stone",
    [2] = "minecraft:glass",
    [3] = "minecraft:chest[facing=north]",
    [4] = "minecraft:chest[facing=south]",
    [5] = "minecraft:chest[facing=west]",
    [6] = "minecraft:chest[facing=east]",
};

static const mc_state_key_entry_t mc_state_key_table[7] = {
    {"minecraft:air", 0u},
    {"minecraft:chest[facing=east]", 6u},
    {"minecraft:chest[facing=north]", 3u},
    {"minecraft:chest[facing=south]", 4u},
    {"minecraft:chest[facing=west]", 5u},
    {"minecraft:glass", 2u},
    {"minecraft:stone", 1u},
};

mc_global_state_id_t mc_global_state_id_from_key(const char *key, mc_global_state_id_t fallback) {
    size_t lo = 0;
    size_t hi = sizeof(mc_state_key_table) / sizeof(mc_state_key_table[0]);

    if (!key) return fallback;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(key, mc_state_key_table[mid].key);
        if (cmp == 0) return mc_state_key_table[mid].id;
        if (cmp < 0) hi = mid;
        else lo = mid + 1;
    }

    return fallback;
}

const char *mc_global_state_key(mc_global_state_id_t id) {
    if ((size_t)id >= GLOBAL_BLOCK_STATES_COUNT) return NULL;
    return mc_state_keys_by_id[id];
}
