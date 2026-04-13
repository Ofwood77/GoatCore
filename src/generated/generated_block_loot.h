#ifndef GENERATED_BLOCK_LOOT_H
#define GENERATED_BLOCK_LOOT_H

#include <stdint.h>
#include "block_registry.h"

#define MC_BLOCK_LOOT_TABLE_SIZE 1168

enum {
    MC_BLOCK_LOOT_FLAG_PRESENT = 1u << 0,
    MC_BLOCK_LOOT_FLAG_SUPPORTED = 1u << 1,
    MC_BLOCK_LOOT_FLAG_FIXED_ITEM = 1u << 2,
    MC_BLOCK_LOOT_FLAG_SILK_TOUCH_FALLBACK = 1u << 3,
    MC_BLOCK_LOOT_FLAG_NO_DROP = 1u << 4,
    MC_BLOCK_LOOT_FLAG_IGNORES_SURVIVES_EXPLOSION = 1u << 5,
    MC_BLOCK_LOOT_FLAG_FORTUNE_FALLBACK = 1u << 6,
    MC_BLOCK_LOOT_FLAG_UNSUPPORTED_COMPLEX = 1u << 7
};

typedef struct {
    int32_t item_id;
    uint16_t count;
    uint16_t flags;
} mc_block_loot_entry_t;

const mc_block_loot_entry_t *mc_block_loot_entry_from_state(int state_id);
int mc_block_loot_default_item_id_from_state(int state_id, int fallback);

#endif /* GENERATED_BLOCK_LOOT_H */
