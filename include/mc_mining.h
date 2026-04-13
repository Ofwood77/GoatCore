#ifndef MC_MINING_H
#define MC_MINING_H

#include <stdbool.h>
#include <stdint.h>

#include "mc_inventory.h"
#include "generated_mining_data.h"

#define MC_MINING_TICK_MS 50
#define MC_MINING_BASE_DESTROY_TICKS 30
#define MC_MINING_BASE_NO_HARVEST_TICKS 100
#define MC_MINING_BREAK_GRACE_MS (MC_MINING_TICK_MS * 2)
#define MC_MINING_UNKNOWN_HARDNESS_X100 100

typedef struct {
    bool known_hardness;
    bool breakable;
    bool instant;
    bool tool_matches;
    bool can_harvest;
    bool requires_correct_tool;
    int32_t hardness_x100;
    uint16_t speed_x100;
    mc_mining_tool_category_t block_category;
    mc_mining_tool_category_t tool_category;
    mc_mining_harvest_level_t required_harvest_level;
    mc_mining_harvest_level_t tool_harvest_level;
    int64_t required_ms;
} mc_mining_break_info_t;

bool mc_mining_state_is_air(int32_t state_id);
mc_mining_break_info_t mc_mining_break_info(int32_t state_id, const mc_slot_t *held_item);
bool mc_mining_elapsed_enough(const mc_mining_break_info_t *info, int64_t started_ms, int64_t now_ms,
                              int64_t *out_elapsed_ms);

#endif /* MC_MINING_H */
