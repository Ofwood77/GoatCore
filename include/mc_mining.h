#ifndef MC_MINING_H
#define MC_MINING_H

#include <stdbool.h>
#include <stdint.h>

#include "mc_inventory.h"

#define MC_MINING_TICK_MS 50
#define MC_MINING_BASE_DESTROY_TICKS 30
#define MC_MINING_UNKNOWN_HARDNESS_X100 100

typedef struct {
    bool known_hardness;
    bool breakable;
    bool instant;
    int32_t hardness_x100;
    int64_t required_ms;
} mc_mining_break_info_t;

bool mc_mining_state_is_air(int32_t state_id);
mc_mining_break_info_t mc_mining_break_info(int32_t state_id, const mc_slot_t *held_item);
bool mc_mining_elapsed_enough(const mc_mining_break_info_t *info, int64_t started_ms, int64_t now_ms,
                              int64_t *out_elapsed_ms);

#endif /* MC_MINING_H */
