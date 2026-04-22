#ifndef MC_MINING_H
#define MC_MINING_H

#include <stdbool.h>
#include <stdint.h>

#include "mc_inventory.h"
#include "generated_mining_data.h"

#define MC_MINING_TICK_MS 50
#define MC_MINING_BASE_DESTROY_TICKS 30
#define MC_MINING_BASE_NO_HARVEST_TICKS 100
#define MC_MINING_BREAK_GRACE_MIN_MS (MC_MINING_TICK_MS * 2)
#define MC_MINING_BREAK_GRACE_MAX_MS 500
#define MC_MINING_BREAK_GRACE_DIVISOR 20
#define MC_MINING_BREAK_GRACE_REQUIRED_DIVISOR 3
#define MC_MINING_UNKNOWN_HARDNESS_X100 100

/* Snapshot of the server's mining decision for one block/tool combination.
 * The important distinction is:
 * - breakable: the block may ever be removed
 * - required_ms: how long the current held item must mine before STOP is valid
 * - can_harvest: whether a successful break may yield the default useful drop */
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

/* Runtime mining session frozen at START_DESTROY_BLOCK. STOP validation should
 * only compare the live world against this snapshot; it should not recompute
 * tool rules from scratch. */
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
/* Builds the full authoritative break/harvest decision for the held item. */
mc_mining_break_info_t mc_mining_break_info(int32_t state_id, const mc_slot_t *held_item);
int64_t mc_mining_break_grace_ms(int64_t required_ms);
int64_t mc_mining_required_elapsed_ms(const mc_mining_break_info_t *info);
bool mc_mining_elapsed_enough(const mc_mining_break_info_t *info, int64_t started_ms, int64_t now_ms,
                              int64_t *out_elapsed_ms);
void mc_mining_session_clear(mc_mining_session_t *session);
void mc_mining_session_start(mc_mining_session_t *session, int32_t x, int32_t y, int32_t z, int32_t state_id,
                             int64_t started_ms, int32_t tool_item_id,
                             const mc_mining_break_info_t *break_info);
/* Returns the reason a STOP packet was refused, which is useful both for logs
 * and for choosing the right resync path in the protocol handler. */
mc_mining_stop_result_t mc_mining_session_validate_stop(const mc_mining_session_t *session, int32_t x, int32_t y,
                                                        int32_t z, int32_t current_state_id,
                                                        int32_t current_tool_item_id, int64_t now_ms,
                                                        int64_t *out_elapsed_ms);
const char *mc_mining_stop_result_name(mc_mining_stop_result_t result);

#endif /* MC_MINING_H */
