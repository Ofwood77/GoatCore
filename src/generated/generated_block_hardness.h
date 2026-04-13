#ifndef GENERATED_BLOCK_HARDNESS_H
#define GENERATED_BLOCK_HARDNESS_H

#include <stdbool.h>
#include <stdint.h>
#include "block_registry.h"

#define MC_BLOCK_HARDNESS_TABLE_SIZE 1168
#define MC_BLOCK_HARDNESS_SCALE 100

enum {
    MC_BLOCK_HARDNESS_FLAG_PRESENT = 1u << 0,
    MC_BLOCK_HARDNESS_FLAG_UNBREAKABLE = 1u << 1,
    MC_BLOCK_HARDNESS_FLAG_INSTANT = 1u << 2
};

typedef struct {
    int16_t hardness_x100;
    uint16_t flags;
} mc_block_hardness_entry_t;

const mc_block_hardness_entry_t *mc_block_hardness_entry_from_state(int state_id);
float mc_block_hardness_from_state(int state_id, float fallback);
bool mc_block_hardness_is_unbreakable(int state_id);

#endif /* GENERATED_BLOCK_HARDNESS_H */
