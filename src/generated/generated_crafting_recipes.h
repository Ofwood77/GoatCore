#ifndef GENERATED_CRAFTING_RECIPES_H
#define GENERATED_CRAFTING_RECIPES_H

#include <stddef.h>
#include <stdint.h>

enum {
    MC_CRAFTING_RECIPE_SHAPED = 1,
    MC_CRAFTING_RECIPE_SHAPELESS = 2
};

typedef struct {
    uint32_t item_offset;
    uint16_t item_count;
} mc_crafting_ingredient_t;

typedef struct {
    uint8_t type;
    uint8_t width;
    uint8_t height;
    uint8_t ingredient_count;
    uint32_t ingredient_offset;
    int32_t result_item_id;
    uint8_t result_count;
} mc_crafting_recipe_t;

extern const mc_crafting_recipe_t MC_CRAFTING_RECIPES[];
extern const size_t MC_CRAFTING_RECIPE_COUNT;
extern const mc_crafting_ingredient_t MC_CRAFTING_INGREDIENTS[];
extern const size_t MC_CRAFTING_INGREDIENT_COUNT;
extern const int32_t MC_CRAFTING_INGREDIENT_ITEMS[];
extern const size_t MC_CRAFTING_INGREDIENT_ITEM_COUNT;

#endif /* GENERATED_CRAFTING_RECIPES_H */
