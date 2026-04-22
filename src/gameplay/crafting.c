/*
 * Server-side crafting matcher and result consumer built on generated recipe
 * tables. play.c owns container protocol, while this file answers narrower
 * questions: "what recipe matches this grid?" and "what changes when the
 * player takes the result?".
 */
#include "mc_crafting.h"

#include <string.h>

#define MC_CRAFTING_MAX_GRID_SLOTS 9
#define MC_CRAFTING_MAX_STACK 64

typedef struct {
    const mc_crafting_recipe_t *recipe;
    int shaped_x;
    int shaped_y;
} mc_crafting_match_info_t;
/* For shaped recipes we keep the matched top-left offset so consumption can
 * replay the exact cells that produced the result. */

static bool ingredient_accepts_item(const mc_crafting_ingredient_t *ingredient, int32_t item_id) {
    if (!ingredient || ingredient->item_count == 0) return false;
    if (ingredient->item_offset + ingredient->item_count > MC_CRAFTING_INGREDIENT_ITEM_COUNT) return false;
    for (uint16_t i = 0; i < ingredient->item_count; i++) {
        if (MC_CRAFTING_INGREDIENT_ITEMS[ingredient->item_offset + i] == item_id) return true;
    }
    return false;
}

static const mc_crafting_ingredient_t *recipe_ingredient(const mc_crafting_recipe_t *recipe, int index) {
    if (!recipe || index < 0 || index >= recipe->ingredient_count) return NULL;
    if (recipe->ingredient_offset + (uint32_t)index >= MC_CRAFTING_INGREDIENT_COUNT) return NULL;
    return &MC_CRAFTING_INGREDIENTS[recipe->ingredient_offset + (uint32_t)index];
}

static bool shaped_matches_at(const mc_crafting_recipe_t *recipe, const mc_slot_t *grid, int width, int height, int ox, int oy) {
    for (int gy = 0; gy < height; gy++) {
        for (int gx = 0; gx < width; gx++) {
            const mc_slot_t *slot = &grid[gy * width + gx];
            bool inside = gx >= ox && gx < ox + recipe->width && gy >= oy && gy < oy + recipe->height;
            const mc_crafting_ingredient_t *ingredient = NULL;
            if (inside) {
                int rx = gx - ox;
                int ry = gy - oy;
                ingredient = recipe_ingredient(recipe, ry * recipe->width + rx);
            }

            if (!ingredient || ingredient->item_count == 0) {
                if (slot->present && slot->count > 0) return false;
                continue;
            }
            if (!slot->present || slot->count <= 0) return false;
            if (!ingredient_accepts_item(ingredient, slot->item_id)) return false;
        }
    }
    return true;
}

static bool match_shaped(const mc_crafting_recipe_t *recipe, const mc_slot_t *grid, int width, int height, mc_crafting_match_info_t *out) {
    if (!recipe || recipe->width <= 0 || recipe->height <= 0) return false;
    if (recipe->width > width || recipe->height > height) return false;
    for (int oy = 0; oy <= height - recipe->height; oy++) {
        for (int ox = 0; ox <= width - recipe->width; ox++) {
            if (!shaped_matches_at(recipe, grid, width, height, ox, oy)) continue;
            if (out) {
                out->recipe = recipe;
                out->shaped_x = ox;
                out->shaped_y = oy;
            }
            return true;
        }
    }
    return false;
}

static int collect_present_slots(const mc_slot_t *grid, int slot_count, int *present) {
    int len = 0;
    for (int i = 0; i < slot_count; i++) {
        if (!grid[i].present || grid[i].count <= 0) continue;
        present[len++] = i;
    }
    return len;
}

static bool shapeless_match_rec(const mc_crafting_recipe_t *recipe, const mc_slot_t *grid, const int *present, int present_count,
                                int ingredient_index, bool *used) {
    if (ingredient_index >= recipe->ingredient_count) return true;

    const mc_crafting_ingredient_t *ingredient = recipe_ingredient(recipe, ingredient_index);
    if (!ingredient || ingredient->item_count == 0) return false;

    /* Shapeless recipes are small (at most 3x3), so a simple backtracking
     * matcher is easier to reason about than a more clever multiset solver. */
    for (int i = 0; i < present_count; i++) {
        int slot_index = present[i];
        if (used[i]) continue;
        if (!ingredient_accepts_item(ingredient, grid[slot_index].item_id)) continue;
        used[i] = true;
        if (shapeless_match_rec(recipe, grid, present, present_count, ingredient_index + 1, used)) return true;
        used[i] = false;
    }
    return false;
}

static bool match_shapeless(const mc_crafting_recipe_t *recipe, const mc_slot_t *grid, int width, int height) {
    int present[MC_CRAFTING_MAX_GRID_SLOTS];
    bool used[MC_CRAFTING_MAX_GRID_SLOTS];
    int slot_count = width * height;
    if (!recipe || slot_count > MC_CRAFTING_MAX_GRID_SLOTS) return false;
    int present_count = collect_present_slots(grid, slot_count, present);
    if (present_count != recipe->ingredient_count) return false;
    memset(used, 0, sizeof(used));
    return shapeless_match_rec(recipe, grid, present, present_count, 0, used);
}

static bool find_match(const mc_slot_t *grid, int width, int height, mc_crafting_match_info_t *out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!grid || width <= 0 || height <= 0 || width > 3 || height > 3 || width * height > MC_CRAFTING_MAX_GRID_SLOTS) return false;
    /* Recipes are pre-generated offline, so matching is a pure table walk with
     * no registry lookups or JSON parsing in the hot path. */
    for (size_t i = 0; i < MC_CRAFTING_RECIPE_COUNT; i++) {
        const mc_crafting_recipe_t *recipe = &MC_CRAFTING_RECIPES[i];
        if (recipe->type == MC_CRAFTING_RECIPE_SHAPED) {
            if (match_shaped(recipe, grid, width, height, out)) return true;
        } else if (recipe->type == MC_CRAFTING_RECIPE_SHAPELESS) {
            if (!match_shapeless(recipe, grid, width, height)) continue;
            if (out) {
                out->recipe = recipe;
                out->shaped_x = 0;
                out->shaped_y = 0;
            }
            return true;
        }
    }
    return false;
}

const mc_crafting_recipe_t *mc_crafting_match_grid(const mc_slot_t *grid, int width, int height, mc_slot_t *out_result) {
    mc_crafting_match_info_t match;
    if (out_result) mc_slot_clear(out_result);
    if (!find_match(grid, width, height, &match) || !match.recipe) return NULL;
    if (out_result && mc_slot_set_simple(out_result, match.recipe->result_item_id, match.recipe->result_count) != 0) return NULL;
    return match.recipe;
}

int mc_crafting_update_result(mc_slot_t *result_slot, const mc_slot_t *grid, int width, int height) {
    if (!result_slot) return -1;
    mc_slot_t result = {0};
    /* Always rebuild the result from the input grid instead of trusting the
     * previous result slot. That keeps resyncs deterministic after any click. */
    (void)mc_crafting_match_grid(grid, width, height, &result);
    if (mc_slot_copy(result_slot, &result) != 0) {
        mc_slot_clear(&result);
        return -1;
    }
    mc_slot_clear(&result);
    return 0;
}

static bool cursor_accepts_result(const mc_slot_t *cursor, const mc_slot_t *result) {
    if (!result || !result->present || result->count <= 0) return false;
    if (!cursor || !cursor->present || cursor->count <= 0) return true;
    if (!mc_slot_is_same_item(cursor, result)) return false;
    return cursor->count + result->count <= MC_CRAFTING_MAX_STACK;
}

static int add_result_to_cursor(mc_slot_t *cursor, const mc_slot_t *result) {
    if (!cursor || !result || !result->present || result->count <= 0) return -1;
    if (!cursor->present || cursor->count <= 0) return mc_slot_copy(cursor, result);
    if (!mc_slot_is_same_item(cursor, result)) return -1;
    if (cursor->count + result->count > MC_CRAFTING_MAX_STACK) return -1;
    cursor->count += result->count;
    return 0;
}

static void consume_slot(mc_slot_t *slot) {
    if (!slot || !slot->present || slot->count <= 0) return;
    slot->count--;
    if (slot->count <= 0) mc_slot_clear(slot);
}

static void consume_shaped(const mc_crafting_match_info_t *match, mc_slot_t *grid, int width) {
    const mc_crafting_recipe_t *recipe = match->recipe;
    for (int ry = 0; ry < recipe->height; ry++) {
        for (int rx = 0; rx < recipe->width; rx++) {
            const mc_crafting_ingredient_t *ingredient = recipe_ingredient(recipe, ry * recipe->width + rx);
            if (!ingredient || ingredient->item_count == 0) continue;
            int gx = match->shaped_x + rx;
            int gy = match->shaped_y + ry;
            consume_slot(&grid[gy * width + gx]);
        }
    }
}

static void consume_shapeless(const mc_crafting_recipe_t *recipe, mc_slot_t *grid, int width, int height) {
    int slot_count = width * height;
    bool used[MC_CRAFTING_MAX_GRID_SLOTS];
    memset(used, 0, sizeof(used));
    for (int ri = 0; ri < recipe->ingredient_count; ri++) {
        const mc_crafting_ingredient_t *ingredient = recipe_ingredient(recipe, ri);
        if (!ingredient) continue;
        for (int si = 0; si < slot_count; si++) {
            if (used[si] || !grid[si].present || grid[si].count <= 0) continue;
            if (!ingredient_accepts_item(ingredient, grid[si].item_id)) continue;
            used[si] = true;
            consume_slot(&grid[si]);
            break;
        }
    }
}

int mc_crafting_take_result(mc_slot_t *result_slot, mc_slot_t *grid, int width, int height, mc_slot_t *cursor_slot) {
    mc_crafting_match_info_t match;
    mc_slot_t result = {0};
    if (!result_slot || !grid || !cursor_slot) return -1;
    /* Re-match at the moment of pickup so the result cannot outlive a stale
     * client prediction or a previous grid configuration. */
    if (!find_match(grid, width, height, &match) || !match.recipe) {
        mc_slot_clear(result_slot);
        return 0;
    }
    if (mc_slot_set_simple(&result, match.recipe->result_item_id, match.recipe->result_count) != 0) return -1;
    if (!cursor_accepts_result(cursor_slot, &result)) {
        mc_slot_clear(&result);
        return 0;
    }
    if (add_result_to_cursor(cursor_slot, &result) != 0) {
        mc_slot_clear(&result);
        return -1;
    }
    if (match.recipe->type == MC_CRAFTING_RECIPE_SHAPED) {
        consume_shaped(&match, grid, width);
    } else {
        consume_shapeless(match.recipe, grid, width, height);
    }
    mc_slot_clear(&result);
    return mc_crafting_update_result(result_slot, grid, width, height) == 0 ? 1 : -1;
}

int mc_crafting_quick_move_result(mc_slot_t *result_slot, mc_slot_t *grid, int width, int height, mc_inventory_t *inventory) {
    int crafted = 0;

    if (!result_slot || !grid || !inventory) return -1;

    for (int safety = 0; safety < 4096; safety++) {
        mc_crafting_match_info_t match;
        mc_slot_t result = {0};
        mc_slot_t moving = {0};
        int absorbed = 0;

        if (!find_match(grid, width, height, &match) || !match.recipe) {
            mc_slot_clear(result_slot);
            return crafted;
        }
        if (match.recipe->ingredient_count <= 0 ||
            mc_slot_set_simple(&result, match.recipe->result_item_id, match.recipe->result_count) != 0) {
            return -1;
        }
        if (!mc_inventory_can_absorb_slot(inventory, &result)) {
            int rc = mc_slot_copy(result_slot, &result);
            mc_slot_clear(&result);
            return rc == 0 ? crafted : -1;
        }
        if (mc_slot_copy(&moving, &result) != 0) {
            mc_slot_clear(&result);
            return -1;
        }
        absorbed = mc_inventory_try_absorb_slot(inventory, &moving);
        if (absorbed < 0 || moving.present || absorbed != result.count) {
            mc_slot_clear(&moving);
            mc_slot_clear(&result);
            return -1;
        }

        if (match.recipe->type == MC_CRAFTING_RECIPE_SHAPED) {
            consume_shaped(&match, grid, width);
        } else {
            consume_shapeless(match.recipe, grid, width, height);
        }

        crafted++;
        mc_slot_clear(&moving);
        mc_slot_clear(&result);
    }

    /* A valid recipe should stop naturally when the grid or inventory no
     * longer permits another craft. Hitting the safety cap means the caller has
     * found a logic bug instead of spinning forever inside the tick. */
    return mc_crafting_update_result(result_slot, grid, width, height) == 0 ? crafted : -1;
}

bool mc_crafting_grid_has_items(const mc_slot_t *grid, int slot_count) {
    if (!grid || slot_count <= 0) return false;
    for (int i = 0; i < slot_count; i++) {
        if (grid[i].present && grid[i].count > 0) return true;
    }
    return false;
}

void mc_crafting_clear_result(mc_slot_t *result_slot) {
    mc_slot_clear(result_slot);
}
