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

typedef struct {
    bool active;
    int32_t x;
    int32_t y;
    int32_t z;
    int32_t state_id;
    int32_t tool_item_id;
    int64_t started_ms;
    mc_mining_break_info_t break_info;
} mc_mining_session_t;

typedef enum {
    MC_MINING_STOP_OK = 0,
    MC_MINING_STOP_NO_SESSION,
    MC_MINING_STOP_TARGET_MISMATCH,
    MC_MINING_STOP_STATE_CHANGED,
    MC_MINING_STOP_TOOL_CHANGED,
    MC_MINING_STOP_UNBREAKABLE,
    MC_MINING_STOP_TOO_EARLY
} mc_mining_stop_result_t;

bool mc_mining_state_is_air(int32_t state_id);
int32_t mc_mining_slot_item_id(const mc_slot_t *slot);
mc_mining_break_info_t mc_mining_break_info(int32_t state_id, const mc_slot_t *held_item);
bool mc_mining_elapsed_enough(const mc_mining_break_info_t *info, int64_t started_ms, int64_t now_ms,
                              int64_t *out_elapsed_ms);
void mc_mining_session_clear(mc_mining_session_t *session);
void mc_mining_session_start(mc_mining_session_t *session, int32_t x, int32_t y, int32_t z, int32_t state_id,
                             int64_t started_ms, int32_t tool_item_id,
                             const mc_mining_break_info_t *break_info);
mc_mining_stop_result_t mc_mining_session_validate_stop(const mc_mining_session_t *session, int32_t x, int32_t y,
                                                        int32_t z, int32_t current_state_id,
                                                        int32_t current_tool_item_id, int64_t now_ms,
                                                        int64_t *out_elapsed_ms);
const char *mc_mining_stop_result_name(mc_mining_stop_result_t result);

#endif /* MC_MINING_H */
