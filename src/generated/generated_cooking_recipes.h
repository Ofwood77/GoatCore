#ifndef GENERATED_COOKING_RECIPES_H
#define GENERATED_COOKING_RECIPES_H

#include <stddef.h>
#include <stdint.h>

enum {
    MC_COOKING_RECIPE_SMELTING = 1,
    MC_COOKING_RECIPE_SMOKING = 2,
    MC_COOKING_RECIPE_BLASTING = 3
};

typedef struct {
    uint8_t type;
    uint32_t ingredient_offset;
    uint16_t ingredient_count;
    int32_t result_item_id;
    uint8_t result_count;
    int16_t cook_time;
} mc_cooking_recipe_t;

typedef struct {
    int32_t item_id;
    int32_t burn_ticks;
    int32_t remainder_item_id;
} mc_fuel_entry_t;

extern const mc_cooking_recipe_t MC_COOKING_RECIPES[];
extern const size_t MC_COOKING_RECIPE_COUNT;
extern const int32_t MC_COOKING_INGREDIENT_ITEMS[];
extern const size_t MC_COOKING_INGREDIENT_ITEM_COUNT;
extern const mc_fuel_entry_t MC_FUEL_ENTRIES[];
extern const size_t MC_FUEL_ENTRY_COUNT;

#endif /* GENERATED_COOKING_RECIPES_H */
