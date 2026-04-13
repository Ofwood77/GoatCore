#include "mc_mining.h"

#include "generated_block_hardness.h"

static bool mining_state_valid(int32_t state_id) {
    return state_id >= 0 && (size_t)state_id < GLOBAL_BLOCK_STATES_COUNT;
}

bool mc_mining_state_is_air(int32_t state_id) {
    if (!mining_state_valid(state_id)) return false;
    return (GLOBAL_BLOCK_STATES[state_id].flags & MC_BLOCK_FLAG_IS_AIR) != 0u;
}

static int64_t required_ms_from_hardness_x100(int32_t hardness_x100) {
    if (hardness_x100 <= 0) return 0;
    int64_t ticks = ((int64_t)hardness_x100 * MC_MINING_BASE_DESTROY_TICKS + MC_BLOCK_HARDNESS_SCALE - 1) /
                    MC_BLOCK_HARDNESS_SCALE;
    if (ticks < 1) ticks = 1;
    return ticks * MC_MINING_TICK_MS;
}

mc_mining_break_info_t mc_mining_break_info(int32_t state_id, const mc_slot_t *held_item) {
    (void)held_item;

    mc_mining_break_info_t info = {0};
    info.hardness_x100 = MC_MINING_UNKNOWN_HARDNESS_X100;
    info.required_ms = required_ms_from_hardness_x100(info.hardness_x100);

    if (!mining_state_valid(state_id)) {
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

    info.breakable = true;
    if (info.hardness_x100 <= 0) {
        info.instant = true;
        info.required_ms = 0;
    } else {
        info.required_ms = required_ms_from_hardness_x100(info.hardness_x100);
    }
    return info;
}

bool mc_mining_elapsed_enough(const mc_mining_break_info_t *info, int64_t started_ms, int64_t now_ms,
                              int64_t *out_elapsed_ms) {
    int64_t elapsed_ms = now_ms - started_ms;
    if (elapsed_ms < 0) elapsed_ms = 0;
    if (out_elapsed_ms) *out_elapsed_ms = elapsed_ms;
    if (!info || !info->breakable) return false;
    return elapsed_ms >= info->required_ms;
}
