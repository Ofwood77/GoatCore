#include "mc_block_drops.h"
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

static void assert_fixed_drop(const char *block_key, const char *item_name) {
    mc_block_drop_t drop = {0};
    assert(mc_block_drop_resolve_default(state_id(block_key), true, 11, 65, -7, 1234, &drop));
    assert(drop.item_id == mc_minecraft_item_id(item_name));
    assert(drop.count == 1);
}

static void assert_drop_range(const char *block_key, const char *item_name, int32_t min_count, int32_t max_count) {
    for (int i = 0; i < 64; i++) {
        mc_block_drop_t drop = {0};
        assert(mc_block_drop_resolve_default(state_id(block_key), true, 11 + i, 65, -7, 1234 + i, &drop));
        assert(drop.item_id == mc_minecraft_item_id(item_name));
        assert(drop.count >= min_count);
        assert(drop.count <= max_count);
    }
}

int main(void) {
    int32_t stone = state_id("minecraft:stone");
    int32_t oak_log = state_id("minecraft:oak_log[axis=y]");
    int32_t iron_ore = state_id("minecraft:iron_ore");
    int32_t diamond_ore = state_id("minecraft:diamond_ore");

    assert(stone >= 0);
    assert(oak_log >= 0);
    assert(iron_ore >= 0);
    assert(diamond_ore >= 0);

    mc_block_drop_t drop = {0};
    assert(!mc_block_drop_resolve_default(stone, false, 0, 64, 0, 1000, &drop));
    assert(drop.item_id < 0);
    assert(drop.count == 0);
    assert(!mc_block_drop_resolve_default(iron_ore, false, 0, 64, 0, 1000, &drop));

    assert_fixed_drop("minecraft:stone", "minecraft:cobblestone");
    assert_fixed_drop("minecraft:oak_log[axis=y]", "minecraft:oak_log");
    assert_fixed_drop("minecraft:iron_ore", "minecraft:raw_iron");
    assert_fixed_drop("minecraft:deepslate_iron_ore", "minecraft:raw_iron");
    assert_fixed_drop("minecraft:gold_ore", "minecraft:raw_gold");
    assert_fixed_drop("minecraft:deepslate_gold_ore", "minecraft:raw_gold");
    assert_fixed_drop("minecraft:diamond_ore", "minecraft:diamond");
    assert_fixed_drop("minecraft:deepslate_diamond_ore", "minecraft:diamond");
    assert_fixed_drop("minecraft:coal_ore", "minecraft:coal");
    assert_fixed_drop("minecraft:deepslate_coal_ore", "minecraft:coal");
    assert_fixed_drop("minecraft:emerald_ore", "minecraft:emerald");
    assert_fixed_drop("minecraft:deepslate_emerald_ore", "minecraft:emerald");
    assert_fixed_drop("minecraft:nether_quartz_ore", "minecraft:quartz");

    assert_drop_range("minecraft:copper_ore", "minecraft:raw_copper", 2, 5);
    assert_drop_range("minecraft:deepslate_copper_ore", "minecraft:raw_copper", 2, 5);
    assert_drop_range("minecraft:lapis_ore", "minecraft:lapis_lazuli", 4, 9);
    assert_drop_range("minecraft:deepslate_lapis_ore", "minecraft:lapis_lazuli", 4, 9);
    assert_drop_range("minecraft:redstone_ore[lit=false]", "minecraft:redstone", 4, 5);
    assert_drop_range("minecraft:deepslate_redstone_ore[lit=false]", "minecraft:redstone", 4, 5);

    mc_slot_t wooden_pickaxe = tool_slot("minecraft:wooden_pickaxe");
    mc_slot_t stone_pickaxe = tool_slot("minecraft:stone_pickaxe");
    mc_mining_break_info_t iron_wood = mc_mining_break_info(iron_ore, &wooden_pickaxe);
    mc_mining_break_info_t iron_stone = mc_mining_break_info(iron_ore, &stone_pickaxe);
    assert(!iron_wood.can_harvest);
    assert(iron_stone.can_harvest);
    assert(!mc_block_drop_resolve_default(iron_ore, iron_wood.can_harvest, 0, 64, 0, 1000, &drop));
    assert(mc_block_drop_resolve_default(iron_ore, iron_stone.can_harvest, 0, 64, 0, 1000, &drop));
    assert(drop.item_id == mc_minecraft_item_id("minecraft:raw_iron"));

    mc_mining_break_info_t diamond_stone = mc_mining_break_info(diamond_ore, &stone_pickaxe);
    assert(!diamond_stone.can_harvest);
    assert(!mc_block_drop_resolve_default(diamond_ore, diamond_stone.can_harvest, 0, 64, 0, 1000, &drop));

    return 0;
}
