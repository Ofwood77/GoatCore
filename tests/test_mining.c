#include "mc_mining.h"

#include "block_registry.h"
#include "generated_minecraft_ids.h"

#include <assert.h>
#include <stdint.h>

static int32_t state_id(const char *key) {
    return (int32_t)mc_global_state_id_from_key(key, UINT32_MAX);
}

static mc_slot_t tool_slot(const char *item_name) {
    int32_t item_id = mc_minecraft_item_id(item_name);
    assert(item_id > 0);
    mc_slot_t slot = {0};
    slot.present = true;
    slot.item_id = item_id;
    slot.count = 1;
    return slot;
}

int main(void) {
    int32_t air = state_id("minecraft:air");
    int32_t dirt = state_id("minecraft:dirt");
    int32_t oak_log = state_id("minecraft:oak_log[axis=y]");
    int32_t stone = state_id("minecraft:stone");
    int32_t iron_ore = state_id("minecraft:iron_ore");
    int32_t diamond_ore = state_id("minecraft:diamond_ore");
    int32_t obsidian = state_id("minecraft:obsidian");
    int32_t bedrock = state_id("minecraft:bedrock");

    assert(air >= 0);
    assert(dirt >= 0);
    assert(oak_log >= 0);
    assert(stone >= 0);
    assert(iron_ore >= 0);
    assert(diamond_ore >= 0);
    assert(obsidian >= 0);
    assert(bedrock >= 0);

    mc_slot_t wooden_pickaxe = tool_slot("minecraft:wooden_pickaxe");
    mc_slot_t stone_pickaxe = tool_slot("minecraft:stone_pickaxe");
    mc_slot_t copper_pickaxe = tool_slot("minecraft:copper_pickaxe");
    mc_slot_t iron_pickaxe = tool_slot("minecraft:iron_pickaxe");
    mc_slot_t wooden_axe = tool_slot("minecraft:wooden_axe");
    mc_slot_t iron_axe = tool_slot("minecraft:iron_axe");
    mc_slot_t stone_hoe = tool_slot("minecraft:stone_hoe");
    mc_slot_t diamond_pickaxe = tool_slot("minecraft:diamond_pickaxe");
    mc_slot_t netherite_pickaxe = tool_slot("minecraft:netherite_pickaxe");
    mc_slot_t wooden_shovel = tool_slot("minecraft:wooden_shovel");

    mc_mining_break_info_t air_info = mc_mining_break_info(air, NULL);
    assert(air_info.known_hardness);
    assert(!air_info.breakable);
    assert(air_info.instant);
    assert(air_info.required_ms == 0);

    mc_mining_break_info_t bedrock_info = mc_mining_break_info(bedrock, NULL);
    assert(bedrock_info.known_hardness);
    assert(!bedrock_info.breakable);
    assert(!mc_mining_elapsed_enough(&bedrock_info, 1000, 100000, NULL));

    mc_mining_break_info_t dirt_info = mc_mining_break_info(dirt, NULL);
    mc_mining_break_info_t stone_info = mc_mining_break_info(stone, NULL);
    assert(dirt_info.breakable);
    assert(stone_info.breakable);
    assert(!dirt_info.requires_correct_tool);
    assert(stone_info.requires_correct_tool);
    assert(dirt_info.can_harvest);
    assert(!stone_info.can_harvest);
    assert(dirt_info.required_ms > 0);
    assert(stone_info.required_ms > dirt_info.required_ms);

    mc_mining_break_info_t oak_hand_info = mc_mining_break_info(oak_log, NULL);
    mc_mining_break_info_t oak_axe_info = mc_mining_break_info(oak_log, &wooden_axe);
    mc_mining_break_info_t oak_pickaxe_info = mc_mining_break_info(oak_log, &wooden_pickaxe);
    assert(oak_hand_info.breakable);
    assert(oak_hand_info.block_category == MC_MINING_TOOL_CATEGORY_AXE);
    assert(!oak_hand_info.requires_correct_tool);
    assert(oak_hand_info.can_harvest);
    assert(oak_axe_info.tool_matches);
    assert(oak_axe_info.can_harvest);
    assert(oak_axe_info.required_ms < oak_hand_info.required_ms);
    assert(!oak_pickaxe_info.tool_matches);
    assert(oak_pickaxe_info.can_harvest);
    assert(oak_pickaxe_info.required_ms == oak_hand_info.required_ms);

    mc_mining_break_info_t stone_wood_info = mc_mining_break_info(stone, &wooden_pickaxe);
    mc_mining_break_info_t stone_shovel_info = mc_mining_break_info(stone, &wooden_shovel);
    mc_mining_break_info_t stone_axe_info = mc_mining_break_info(stone, &iron_axe);
    mc_mining_break_info_t stone_hoe_info = mc_mining_break_info(stone, &stone_hoe);
    assert(stone_wood_info.tool_matches);
    assert(stone_wood_info.can_harvest);
    assert(!stone_shovel_info.tool_matches);
    assert(!stone_shovel_info.can_harvest);
    assert(!stone_axe_info.tool_matches);
    assert(!stone_axe_info.can_harvest);
    assert(!stone_hoe_info.tool_matches);
    assert(!stone_hoe_info.can_harvest);
    assert(stone_wood_info.required_ms < stone_info.required_ms);
    assert(stone_shovel_info.required_ms == stone_info.required_ms);
    assert(stone_axe_info.required_ms == stone_info.required_ms);
    assert(stone_hoe_info.required_ms == stone_info.required_ms);

    mc_mining_break_info_t dirt_shovel_info = mc_mining_break_info(dirt, &wooden_shovel);
    assert(dirt_shovel_info.tool_matches);
    assert(dirt_shovel_info.can_harvest);
    assert(dirt_shovel_info.required_ms < dirt_info.required_ms);

    mc_mining_break_info_t iron_ore_hand_info = mc_mining_break_info(iron_ore, NULL);
    mc_mining_break_info_t iron_ore_wood_info = mc_mining_break_info(iron_ore, &wooden_pickaxe);
    mc_mining_break_info_t iron_ore_stone_info = mc_mining_break_info(iron_ore, &stone_pickaxe);
    mc_mining_break_info_t iron_ore_copper_info = mc_mining_break_info(iron_ore, &copper_pickaxe);
    mc_mining_break_info_t iron_ore_wrong_tool_info = mc_mining_break_info(iron_ore, &iron_axe);
    assert(!iron_ore_hand_info.can_harvest);
    assert(!iron_ore_wood_info.can_harvest);
    assert(iron_ore_stone_info.can_harvest);
    assert(iron_ore_copper_info.can_harvest);
    assert(iron_ore_wood_info.tool_matches);
    assert(!iron_ore_wrong_tool_info.tool_matches);
    assert(!iron_ore_wrong_tool_info.can_harvest);
    assert(iron_ore_wood_info.required_ms < iron_ore_hand_info.required_ms);
    assert(iron_ore_wrong_tool_info.required_ms == iron_ore_hand_info.required_ms);

    mc_mining_break_info_t diamond_stone_info = mc_mining_break_info(diamond_ore, &stone_pickaxe);
    mc_mining_break_info_t diamond_iron_info = mc_mining_break_info(diamond_ore, &iron_pickaxe);
    assert(!diamond_stone_info.can_harvest);
    assert(diamond_iron_info.can_harvest);

    mc_mining_break_info_t obsidian_iron_info = mc_mining_break_info(obsidian, &iron_pickaxe);
    mc_mining_break_info_t obsidian_diamond_info = mc_mining_break_info(obsidian, &diamond_pickaxe);
    mc_mining_break_info_t obsidian_netherite_info = mc_mining_break_info(obsidian, &netherite_pickaxe);
    assert(!obsidian_iron_info.can_harvest);
    assert(obsidian_diamond_info.can_harvest);
    assert(obsidian_netherite_info.can_harvest);
    assert(obsidian_iron_info.required_ms > obsidian_diamond_info.required_ms);
    assert(obsidian_netherite_info.required_ms < obsidian_diamond_info.required_ms);
    assert(obsidian_diamond_info.required_ms > 9000);
    assert(obsidian_diamond_info.required_ms < 10000);

    int64_t elapsed_ms = 0;
    int64_t stone_accept_ms = stone_wood_info.required_ms - MC_MINING_BREAK_GRACE_MS;
    assert(stone_accept_ms > 0);
    assert(!mc_mining_elapsed_enough(&stone_wood_info, 1000, 1000 + stone_accept_ms - 1, &elapsed_ms));
    assert(elapsed_ms == stone_accept_ms - 1);
    assert(mc_mining_elapsed_enough(&stone_wood_info, 1000, 1000 + stone_accept_ms, &elapsed_ms));
    assert(elapsed_ms == stone_accept_ms);

    int64_t obsidian_accept_ms = obsidian_diamond_info.required_ms - MC_MINING_BREAK_GRACE_MS;
    assert(obsidian_accept_ms > 0);
    assert(!mc_mining_elapsed_enough(&obsidian_diamond_info, 1000, 1000 + obsidian_accept_ms - 1, &elapsed_ms));
    assert(elapsed_ms == obsidian_accept_ms - 1);
    assert(mc_mining_elapsed_enough(&obsidian_diamond_info, 1000, 1000 + obsidian_accept_ms, &elapsed_ms));
    assert(elapsed_ms == obsidian_accept_ms);

    mc_mining_break_info_t invalid_info = mc_mining_break_info(-1, NULL);
    assert(!invalid_info.breakable);

    return 0;
}
