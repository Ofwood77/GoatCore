#ifndef GENERATED_MINING_DATA_H
#define GENERATED_MINING_DATA_H

#include <stdbool.h>
#include <stdint.h>
#include "block_registry.h"
#include "generated_minecraft_ids.h"

#define MC_MINING_BLOCK_TOOL_TABLE_SIZE 1168
#define MC_MINING_TOOL_SPEED_SCALE 100

typedef enum {
    MC_MINING_TOOL_CATEGORY_NONE = 0,
    MC_MINING_TOOL_CATEGORY_PICKAXE = 1,
    MC_MINING_TOOL_CATEGORY_AXE = 2,
    MC_MINING_TOOL_CATEGORY_SHOVEL = 3,
    MC_MINING_TOOL_CATEGORY_HOE = 4
} mc_mining_tool_category_t;

typedef enum {
    MC_MINING_HARVEST_LEVEL_NONE = 0,
    MC_MINING_HARVEST_LEVEL_STONE = 1,
    MC_MINING_HARVEST_LEVEL_IRON = 2,
    MC_MINING_HARVEST_LEVEL_DIAMOND = 3
} mc_mining_harvest_level_t;

typedef enum {
    MC_MINING_TOOL_MATERIAL_NONE = 0,
    MC_MINING_TOOL_MATERIAL_WOOD = 1,
    MC_MINING_TOOL_MATERIAL_STONE = 2,
    MC_MINING_TOOL_MATERIAL_COPPER = 3,
    MC_MINING_TOOL_MATERIAL_IRON = 4,
    MC_MINING_TOOL_MATERIAL_GOLD = 5,
    MC_MINING_TOOL_MATERIAL_DIAMOND = 6,
    MC_MINING_TOOL_MATERIAL_NETHERITE = 7
} mc_mining_tool_material_t;

enum {
    MC_MINING_BLOCK_TOOL_FLAG_PRESENT = 1u << 0
};

typedef struct {
    uint8_t category;
    uint8_t required_level;
    uint16_t flags;
} mc_mining_block_tool_entry_t;

typedef struct {
    uint8_t category;
    uint8_t harvest_level;
    uint8_t material;
    uint8_t reserved0;
    uint16_t speed_x100;
    uint16_t durability;
} mc_mining_tool_item_entry_t;

const mc_mining_block_tool_entry_t *mc_mining_block_tool_entry_from_state(int state_id);
const mc_mining_tool_item_entry_t *mc_mining_tool_item_entry_from_item(int32_t item_id);
const char *mc_mining_tool_category_name(mc_mining_tool_category_t category);

#endif /* GENERATED_MINING_DATA_H */
