#ifndef MC_CRAFTING_H
#define MC_CRAFTING_H

#include <stdbool.h>
#include "mc_inventory.h"
#include "generated_crafting_recipes.h"

const mc_crafting_recipe_t *mc_crafting_match_grid(const mc_slot_t *grid, int width, int height, mc_slot_t *out_result);
int mc_crafting_update_result(mc_slot_t *result_slot, const mc_slot_t *grid, int width, int height);
int mc_crafting_take_result(mc_slot_t *result_slot, mc_slot_t *grid, int width, int height, mc_slot_t *cursor_slot);
int mc_crafting_quick_move_result(mc_slot_t *result_slot, mc_slot_t *grid, int width, int height, mc_inventory_t *inventory);
bool mc_crafting_grid_has_items(const mc_slot_t *grid, int slot_count);
void mc_crafting_clear_result(mc_slot_t *result_slot);

#endif /* MC_CRAFTING_H */
