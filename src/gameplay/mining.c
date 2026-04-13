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

static int32_t held_item_id(const mc_slot_t *held_item) {
    if (!held_item || !held_item->present || held_item->count <= 0 || held_item->item_id <= 0) return -1;
    return held_item->item_id;
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

    const mc_mining_tool_item_entry_t *tool = mc_mining_tool_item_entry_from_item(held_item_id(held_item));
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
