#ifndef MC_FURNACE_H
#define MC_FURNACE_H

#include <stdbool.h>
#include <stdint.h>

#include "block_entity_store.h"
#include "mc_inventory.h"
#include "generated_cooking_recipes.h"

#define MC_FURNACE_INPUT_SLOT 0
#define MC_FURNACE_FUEL_SLOT 1
#define MC_FURNACE_OUTPUT_SLOT 2
#define MC_FURNACE_SLOT_COUNT 3

typedef enum {
    MC_FURNACE_MACHINE_NONE = 0,
    MC_FURNACE_MACHINE_FURNACE = 1,
    MC_FURNACE_MACHINE_SMOKER = 2,
    MC_FURNACE_MACHINE_BLAST_FURNACE = 3
} mc_furnace_machine_t;

bool mc_furnace_container_kind_is_machine(mc_container_kind_t kind);
mc_furnace_machine_t mc_furnace_machine_for_container_kind(mc_container_kind_t kind);
const mc_cooking_recipe_t *mc_furnace_find_recipe(mc_furnace_machine_t machine, int32_t input_item_id);
int32_t mc_furnace_fuel_burn_ticks(int32_t item_id);
int32_t mc_furnace_fuel_remainder_item_id(int32_t item_id);
int mc_furnace_tick(mc_container_instance_t *container, mc_furnace_machine_t machine);
int mc_furnace_tick_block_entity(mc_block_entity_t *entity, mc_furnace_machine_t machine);

#endif /* MC_FURNACE_H */
