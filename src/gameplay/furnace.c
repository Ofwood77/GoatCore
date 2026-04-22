/*
 * Furnace/smoker/blast-furnace gameplay rules shared by open container windows
 * and block-entity ticking. The core state machine lives here so both paths
 * agree on burn time, cook progress, and output rules.
 */
#include "mc_furnace.h"

#define MC_FURNACE_MAX_STACK 64

bool mc_furnace_container_kind_is_machine(mc_container_kind_t kind) {
    return kind == MC_CONTAINER_KIND_FURNACE || kind == MC_CONTAINER_KIND_SMOKER || kind == MC_CONTAINER_KIND_BLAST_FURNACE;
}

mc_furnace_machine_t mc_furnace_machine_for_container_kind(mc_container_kind_t kind) {
    switch (kind) {
        case MC_CONTAINER_KIND_FURNACE: return MC_FURNACE_MACHINE_FURNACE;
        case MC_CONTAINER_KIND_SMOKER: return MC_FURNACE_MACHINE_SMOKER;
        case MC_CONTAINER_KIND_BLAST_FURNACE: return MC_FURNACE_MACHINE_BLAST_FURNACE;
        default: return MC_FURNACE_MACHINE_NONE;
    }
}

static uint8_t cooking_type_for_machine(mc_furnace_machine_t machine) {
    switch (machine) {
        case MC_FURNACE_MACHINE_FURNACE: return MC_COOKING_RECIPE_SMELTING;
        case MC_FURNACE_MACHINE_SMOKER: return MC_COOKING_RECIPE_SMOKING;
        case MC_FURNACE_MACHINE_BLAST_FURNACE: return MC_COOKING_RECIPE_BLASTING;
        default: return 0;
    }
}

static bool recipe_accepts_item(const mc_cooking_recipe_t *recipe, int32_t item_id) {
    if (!recipe || recipe->ingredient_count == 0) return false;
    if (recipe->ingredient_offset + recipe->ingredient_count > MC_COOKING_INGREDIENT_ITEM_COUNT) return false;
    for (uint16_t i = 0; i < recipe->ingredient_count; i++) {
        if (MC_COOKING_INGREDIENT_ITEMS[recipe->ingredient_offset + i] == item_id) return true;
    }
    return false;
}

const mc_cooking_recipe_t *mc_furnace_find_recipe(mc_furnace_machine_t machine, int32_t input_item_id) {
    uint8_t type = cooking_type_for_machine(machine);
    if (type == 0 || input_item_id <= 0) return NULL;
    for (size_t i = 0; i < MC_COOKING_RECIPE_COUNT; i++) {
        const mc_cooking_recipe_t *recipe = &MC_COOKING_RECIPES[i];
        if (recipe->type != type) continue;
        if (recipe_accepts_item(recipe, input_item_id)) return recipe;
    }
    return NULL;
}

static const mc_fuel_entry_t *fuel_entry_for_item(int32_t item_id) {
    if (item_id <= 0) return NULL;
    for (size_t i = 0; i < MC_FUEL_ENTRY_COUNT; i++) {
        if (MC_FUEL_ENTRIES[i].item_id == item_id) return &MC_FUEL_ENTRIES[i];
    }
    return NULL;
}

int32_t mc_furnace_fuel_burn_ticks(int32_t item_id) {
    const mc_fuel_entry_t *entry = fuel_entry_for_item(item_id);
    return entry ? entry->burn_ticks : 0;
}

int32_t mc_furnace_fuel_remainder_item_id(int32_t item_id) {
    const mc_fuel_entry_t *entry = fuel_entry_for_item(item_id);
    return entry ? entry->remainder_item_id : -1;
}

static bool output_accepts_recipe(const mc_slot_t *output, const mc_cooking_recipe_t *recipe) {
    if (!recipe || recipe->result_item_id <= 0 || recipe->result_count <= 0) return false;
    if (!output || !output->present || output->count <= 0) return true;
    if (output->item_id != recipe->result_item_id) return false;
    return output->count + recipe->result_count <= MC_FURNACE_MAX_STACK;
}

static void consume_one(mc_slot_t *slot) {
    if (!slot || !slot->present || slot->count <= 0) return;
    slot->count--;
    if (slot->count <= 0) mc_slot_clear(slot);
}

static int consume_one_fuel(mc_slot_t *fuel, int32_t remainder_item_id) {
    if (!fuel || !fuel->present || fuel->count <= 0) return 0;
    fuel->count--;
    if (fuel->count <= 0) {
        if (remainder_item_id > 0) return mc_slot_set_simple(fuel, remainder_item_id, 1);
        mc_slot_clear(fuel);
    }
    return 0;
}

static int add_recipe_output(mc_slot_t *output, const mc_cooking_recipe_t *recipe) {
    if (!output || !recipe) return -1;
    if (!output->present || output->count <= 0) {
        return mc_slot_set_simple(output, recipe->result_item_id, recipe->result_count);
    }
    if (output->item_id != recipe->result_item_id) return -1;
    if (output->count + recipe->result_count > MC_FURNACE_MAX_STACK) return -1;
    output->count += recipe->result_count;
    return 0;
}

int mc_furnace_tick(mc_container_instance_t *container, mc_furnace_machine_t machine) {
    if (!container || machine == MC_FURNACE_MACHINE_NONE || container->slot_count < MC_FURNACE_SLOT_COUNT) return 0;

    mc_slot_t *input = &container->slots[MC_FURNACE_INPUT_SLOT];
    mc_slot_t *fuel = &container->slots[MC_FURNACE_FUEL_SLOT];
    mc_slot_t *output = &container->slots[MC_FURNACE_OUTPUT_SLOT];
    const mc_cooking_recipe_t *recipe = NULL;
    bool dirty = false;

    /* burn_time counts down every tick once fuel has been accepted. The pair
     * burn_time/burn_duration is kept so the UI can render a stable progress
     * bar without reverse-engineering the last fuel item. */
    if (container->furnace_burn_time > 0) {
        container->furnace_burn_time--;
        dirty = true;
    }

    if (input->present && input->count > 0) {
        recipe = mc_furnace_find_recipe(machine, input->item_id);
    }

    bool can_cook = recipe && output_accepts_recipe(output, recipe);

    if (container->furnace_burn_time <= 0 && can_cook && fuel->present && fuel->count > 0) {
        int32_t burn_ticks = mc_furnace_fuel_burn_ticks(fuel->item_id);
        if (burn_ticks > 0) {
            int32_t remainder = mc_furnace_fuel_remainder_item_id(fuel->item_id);
            if (consume_one_fuel(fuel, remainder) != 0) return -1;
            container->furnace_burn_time = burn_ticks;
            container->furnace_burn_duration = burn_ticks;
            dirty = true;
        }
    }

    if (can_cook && container->furnace_burn_time > 0) {
        int32_t cook_duration = recipe->cook_time > 0 ? recipe->cook_time : 200;
        if (container->furnace_cook_duration != cook_duration) {
            container->furnace_cook_duration = cook_duration;
            dirty = true;
        }
        /* cook_time only advances while the machine is both lit and able to
         * accept output. The moment output blocks, progress snaps back to the
         * current vanilla-like baseline instead of being left half-valid. */
        container->furnace_cook_time++;
        dirty = true;
        if (container->furnace_cook_time >= container->furnace_cook_duration) {
            if (add_recipe_output(output, recipe) != 0) return -1;
            consume_one(input);
            container->furnace_cook_time = 0;
            dirty = true;
        }
    } else {
        if (container->furnace_cook_time != 0) {
            container->furnace_cook_time = 0;
            dirty = true;
        }
        if (recipe) {
            int32_t cook_duration = recipe->cook_time > 0 ? recipe->cook_time : 200;
            if (container->furnace_cook_duration != cook_duration) {
                container->furnace_cook_duration = cook_duration;
                dirty = true;
            }
        } else if (container->furnace_cook_duration != 0) {
            container->furnace_cook_duration = 0;
            dirty = true;
        }
    }

    if (container->furnace_burn_time <= 0 && !fuel->present && container->furnace_burn_duration != 0) {
        container->furnace_burn_duration = 0;
        dirty = true;
    }

    if (dirty) {
        container->dirty = true;
        container->state_id++;
        return 1;
    }
    return 0;
}

static mc_container_kind_t container_kind_for_machine(mc_furnace_machine_t machine) {
    switch (machine) {
        case MC_FURNACE_MACHINE_FURNACE: return MC_CONTAINER_KIND_FURNACE;
        case MC_FURNACE_MACHINE_SMOKER: return MC_CONTAINER_KIND_SMOKER;
        case MC_FURNACE_MACHINE_BLAST_FURNACE: return MC_CONTAINER_KIND_BLAST_FURNACE;
        default: return MC_CONTAINER_KIND_NONE;
    }
}

int mc_furnace_tick_block_entity(mc_block_entity_t *entity, mc_furnace_machine_t machine) {
    if (!entity || machine == MC_FURNACE_MACHINE_NONE) return 0;

    /* Reuse the container tick logic by copying the persisted block-entity
     * state into a temporary runtime container, then write it back only if the
     * tick produced an observable change. */
    mc_container_instance_t container;
    mc_container_instance_init(&container, container_kind_for_machine(machine), 0, 0, 0);
    container.slot_count = MC_FURNACE_SLOT_COUNT;
    container.furnace_burn_time = entity->data.container.furnace_burn_time;
    container.furnace_burn_duration = entity->data.container.furnace_burn_duration;
    container.furnace_cook_time = entity->data.container.furnace_cook_time;
    container.furnace_cook_duration = entity->data.container.furnace_cook_duration;

    uint32_t slot_count = entity->data.container.slot_count;
    if (slot_count > MC_FURNACE_SLOT_COUNT) slot_count = MC_FURNACE_SLOT_COUNT;
    for (uint32_t i = 0; i < slot_count; i++) {
        if (mc_slot_copy(&container.slots[i], &entity->data.container.slots[i]) != 0) {
            mc_container_instance_clear(&container);
            return -1;
        }
    }

    int rc = mc_furnace_tick(&container, machine);
    if (rc > 0) {
        mc_slot_t next_slots[MC_FURNACE_SLOT_COUNT] = {0};
        for (uint32_t i = 0; i < MC_FURNACE_SLOT_COUNT; i++) {
            if (mc_slot_copy(&next_slots[i], &container.slots[i]) != 0) {
                for (uint32_t j = 0; j < i; j++) mc_slot_clear(&next_slots[j]);
                mc_container_instance_clear(&container);
                return -1;
            }
        }
        uint32_t old_slot_count = entity->data.container.slot_count;
        if (old_slot_count > MC_CONTAINER_SLOT_COUNT) old_slot_count = MC_CONTAINER_SLOT_COUNT;
        for (uint32_t i = 0; i < old_slot_count; i++) mc_slot_clear(&entity->data.container.slots[i]);
        entity->data.container.slot_count = MC_FURNACE_SLOT_COUNT;
        entity->data.container.furnace_burn_time = container.furnace_burn_time;
        entity->data.container.furnace_burn_duration = container.furnace_burn_duration;
        entity->data.container.furnace_cook_time = container.furnace_cook_time;
        entity->data.container.furnace_cook_duration = container.furnace_cook_duration;
        for (uint32_t i = 0; i < MC_FURNACE_SLOT_COUNT; i++) {
            mc_slot_move(&entity->data.container.slots[i], &next_slots[i]);
        }
    }
    mc_container_instance_clear(&container);
    return rc;
}
