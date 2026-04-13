#ifndef GENERATED_ITEM_FOOD_H
#define GENERATED_ITEM_FOOD_H

#include <stdint.h>

#define MC_ITEM_FOOD_TABLE_SIZE 1506

enum {
    MC_ITEM_FOOD_FLAG_PRESENT = 1u << 0,
    MC_ITEM_FOOD_FLAG_ALWAYS_EDIBLE = 1u << 1,
    MC_ITEM_FOOD_FLAG_HAS_REMAINDER = 1u << 2,
    MC_ITEM_FOOD_FLAG_IGNORES_EFFECTS = 1u << 3,
};

typedef struct {
    int16_t nutrition;
    float saturation;
    int32_t remainder_item_id;
    uint16_t flags;
} mc_item_food_entry_t;

const mc_item_food_entry_t *mc_item_food_entry(int32_t item_id);

#endif /* GENERATED_ITEM_FOOD_H */
