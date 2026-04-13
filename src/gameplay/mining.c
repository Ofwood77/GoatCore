#include "mc_mining.h"

#include "generated_block_hardness.h"

static bool mining_state_valid(int32_t state_id) {
    return state_id >= 0 && (size_t)state_id < GLOBAL_BLOCK_STATES_COUNT;
}

bool mc_mining_state_is_air(int32_t state_id) {
    if (!mining_state_valid(state_id)) return false;
    return (GLOBAL_BLOCK_STATES[state_id].flags & MC_BLOCK_FLAG_IS_AIR) != 0u;
}

static int64_t required_ms_from_hardness_x100(int32_t hardness_x100, uint16_t speed_x100, int base_destroy_ticks) {
    if (hardness_x100 <= 0) return 0;
    if (speed_x100 < MC_MINING_TOOL_SPEED_SCALE) speed_x100 = MC_MINING_TOOL_SPEED_SCALE;
    if (base_destroy_ticks <= 0) base_destroy_ticks = MC_MINING_BASE_DESTROY_TICKS;
    int64_t numerator = (int64_t)hardness_x100 * base_destroy_ticks * MC_MINING_TOOL_SPEED_SCALE;
    int64_t denominator = (int64_t)MC_BLOCK_HARDNESS_SCALE * speed_x100;
    int64_t ticks = (numerator + denominator - 1) / denominator;
    if (ticks < 1) ticks = 1;
    return ticks * MC_MINING_TICK_MS;
}

int32_t mc_mining_slot_item_id(const mc_slot_t *slot) {
    if (!slot || !slot->present || slot->count <= 0 || slot->item_id <= 0) return -1;
    return slot->item_id;
}

mc_mining_break_info_t mc_mining_break_info(int32_t state_id, const mc_slot_t *held_item) {
    mc_mining_break_info_t info = {0};
    info.hardness_x100 = MC_MINING_UNKNOWN_HARDNESS_X100;
    info.speed_x100 = MC_MINING_TOOL_SPEED_SCALE;
    info.can_harvest = true;
    info.required_ms =
        required_ms_from_hardness_x100(info.hardness_x100, info.speed_x100, MC_MINING_BASE_DESTROY_TICKS);

    if (!mining_state_valid(state_id)) {
        info.can_harvest = false;
        return info;
    }

    if (mc_mining_state_is_air(state_id)) {
        info.known_hardness = true;
        info.breakable = false;
        info.instant = true;
        info.hardness_x100 = 0;
        info.required_ms = 0;
        return info;
    }

    const mc_block_hardness_entry_t *entry = mc_block_hardness_entry_from_state(state_id);
    if (entry && (entry->flags & MC_BLOCK_HARDNESS_FLAG_PRESENT) != 0u) {
        info.known_hardness = true;
        info.hardness_x100 = entry->hardness_x100;
        info.instant = (entry->flags & MC_BLOCK_HARDNESS_FLAG_INSTANT) != 0u;
        if ((entry->flags & MC_BLOCK_HARDNESS_FLAG_UNBREAKABLE) != 0u) {
            info.breakable = false;
            info.required_ms = 0;
            return info;
        }
    }

    const mc_mining_block_tool_entry_t *block_tool = mc_mining_block_tool_entry_from_state(state_id);
    if (block_tool && (block_tool->flags & MC_MINING_BLOCK_TOOL_FLAG_PRESENT) != 0u) {
        info.block_category = (mc_mining_tool_category_t)block_tool->category;
        info.required_harvest_level = (mc_mining_harvest_level_t)block_tool->required_level;
        info.requires_correct_tool =
            (block_tool->flags & MC_MINING_BLOCK_TOOL_FLAG_REQUIRES_CORRECT_TOOL) != 0u;
    }

    const mc_mining_tool_item_entry_t *tool = mc_mining_tool_item_entry_from_item(mc_mining_slot_item_id(held_item));
    if (tool) {
        info.tool_category = (mc_mining_tool_category_t)tool->category;
        info.tool_harvest_level = (mc_mining_harvest_level_t)tool->harvest_level;
    }

    info.tool_matches = info.block_category != MC_MINING_TOOL_CATEGORY_NONE && info.tool_category == info.block_category;
    if (info.tool_matches && tool && tool->speed_x100 > info.speed_x100) {
        info.speed_x100 = tool->speed_x100;
    }
    if (info.requires_correct_tool) {
        info.can_harvest = info.tool_matches && info.tool_harvest_level >= info.required_harvest_level;
    }

    info.breakable = true;
    if (info.hardness_x100 <= 0) {
        info.instant = true;
        info.required_ms = 0;
    } else {
        int base_destroy_ticks = info.requires_correct_tool && !info.can_harvest ? MC_MINING_BASE_NO_HARVEST_TICKS
                                                                                 : MC_MINING_BASE_DESTROY_TICKS;
        info.required_ms = required_ms_from_hardness_x100(info.hardness_x100, info.speed_x100, base_destroy_ticks);
    }
    return info;
}

bool mc_mining_elapsed_enough(const mc_mining_break_info_t *info, int64_t started_ms, int64_t now_ms,
                              int64_t *out_elapsed_ms) {
    int64_t elapsed_ms = now_ms - started_ms;
    if (elapsed_ms < 0) elapsed_ms = 0;
    if (out_elapsed_ms) *out_elapsed_ms = elapsed_ms;
    if (!info || !info->breakable) return false;
    int64_t required_ms = info->required_ms;
    if (required_ms > MC_MINING_BREAK_GRACE_MS) required_ms -= MC_MINING_BREAK_GRACE_MS;
    return elapsed_ms >= required_ms;
}

void mc_mining_session_clear(mc_mining_session_t *session) {
    if (!session) return;
    *session = (mc_mining_session_t){0};
    session->state_id = -1;
    session->tool_item_id = -1;
}

void mc_mining_session_start(mc_mining_session_t *session, int32_t x, int32_t y, int32_t z, int32_t state_id,
                             int64_t started_ms, int32_t tool_item_id,
                             const mc_mining_break_info_t *break_info) {
    if (!session) return;
    mc_mining_session_clear(session);
    if (!break_info || !break_info->breakable) return;
    session->active = true;
    session->x = x;
    session->y = y;
    session->z = z;
    session->state_id = state_id;
    session->tool_item_id = tool_item_id;
    session->started_ms = started_ms;
    session->break_info = *break_info;
}

mc_mining_stop_result_t mc_mining_session_validate_stop(const mc_mining_session_t *session, int32_t x, int32_t y,
                                                        int32_t z, int32_t current_state_id,
                                                        int32_t current_tool_item_id, int64_t now_ms,
                                                        int64_t *out_elapsed_ms) {
    if (!session || !session->active) return MC_MINING_STOP_NO_SESSION;
    if (session->x != x || session->y != y || session->z != z) return MC_MINING_STOP_TARGET_MISMATCH;
    if (session->state_id != current_state_id) return MC_MINING_STOP_STATE_CHANGED;
    if (session->tool_item_id != current_tool_item_id) return MC_MINING_STOP_TOOL_CHANGED;
    if (!session->break_info.breakable) return MC_MINING_STOP_UNBREAKABLE;
    if (!mc_mining_elapsed_enough(&session->break_info, session->started_ms, now_ms, out_elapsed_ms)) {
        return MC_MINING_STOP_TOO_EARLY;
    }
    return MC_MINING_STOP_OK;
}

const char *mc_mining_stop_result_name(mc_mining_stop_result_t result) {
    switch (result) {
        case MC_MINING_STOP_OK: return "ok";
        case MC_MINING_STOP_NO_SESSION: return "no_session";
        case MC_MINING_STOP_TARGET_MISMATCH: return "target_mismatch";
        case MC_MINING_STOP_STATE_CHANGED: return "state_changed";
        case MC_MINING_STOP_TOOL_CHANGED: return "tool_changed";
        case MC_MINING_STOP_UNBREAKABLE: return "unbreakable";
        case MC_MINING_STOP_TOO_EARLY: return "too_early";
        default: return "unknown";
    }
}
