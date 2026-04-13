#include "mc_inventory.h"
#include "mc_protocol.h"
#include "generated_minecraft_ids.h"

#include <stdlib.h>
#include <string.h>

static int read_varint_at(const uint8_t *buf, size_t len, size_t *pos, int32_t *out) {
    size_t n = 0;
    if (!buf || !pos || !out || *pos >= len) return -1;
    if (varint_read(buf + *pos, len - *pos, out, &n) != 0) return -1;
    *pos += n;
    return 0;
}

static int write_varint_at(uint8_t *buf, size_t cap, size_t *pos, int32_t value) {
    size_t n = 0;
    if (!buf || !pos || *pos > cap) return -1;
    if (varint_write(buf + *pos, cap - *pos, value, &n) != 0) return -1;
    *pos += n;
    return 0;
}

void mc_slot_init(mc_slot_t *slot) {
    if (!slot) return;
    memset(slot, 0, sizeof(*slot));
}

void mc_slot_clear(mc_slot_t *slot) {
    if (!slot) return;
    free(slot->components);
    memset(slot, 0, sizeof(*slot));
}

void mc_slot_move(mc_slot_t *dst, mc_slot_t *src) {
    if (!dst || !src || dst == src) return;
    mc_slot_clear(dst);
    *dst = *src;
    memset(src, 0, sizeof(*src));
}

int mc_slot_copy(mc_slot_t *dst, const mc_slot_t *src) {
    if (!dst || !src) return -1;
    mc_slot_t tmp = {0};
    tmp = *src;
    tmp.components = NULL;
    if (src->components_len > 0) {
        tmp.components = (uint8_t *)malloc(src->components_len);
        if (!tmp.components) return -1;
        memcpy(tmp.components, src->components, src->components_len);
    }
    mc_slot_clear(dst);
    *dst = tmp;
    return 0;
}

bool mc_slot_is_same_item(const mc_slot_t *a, const mc_slot_t *b) {
    if (!a || !b) return false;
    if (a->present != b->present) return false;
    if (!a->present) return true;
    return a->item_id == b->item_id && a->damage == b->damage;
}

int mc_slot_set_simple(mc_slot_t *slot, int32_t item_id, int32_t count) {
    if (!slot) return -1;
    mc_slot_clear(slot);
    if (item_id <= 0 || count <= 0) return 0;
    slot->present = true;
    slot->item_id = item_id;
    slot->count = count;
    return 0;
}

void mc_inventory_init(mc_inventory_t *inv) {
    if (!inv) return;
    memset(inv, 0, sizeof(*inv));
}

void mc_inventory_clear(mc_inventory_t *inv) {
    if (!inv) return;
    for (size_t i = 0; i < MC_PLAYER_SLOT_COUNT; i++) mc_slot_clear(&inv->slots[i]);
    mc_slot_clear(&inv->cursor_slot);
    memset(inv, 0, sizeof(*inv));
}

void mc_container_instance_init(mc_container_instance_t *container, mc_container_kind_t kind, int32_t x, int32_t y, int32_t z) {
    if (!container) return;
    memset(container, 0, sizeof(*container));
    container->kind = kind;
    container->x = x;
    container->y = y;
    container->z = z;
    container->slot_count = MC_CONTAINER_SLOT_COUNT;
    if (kind == MC_CONTAINER_KIND_FURNACE || kind == MC_CONTAINER_KIND_SMOKER || kind == MC_CONTAINER_KIND_BLAST_FURNACE) {
        container->slot_count = 3;
    }
    container->state_id = 1;
}

void mc_container_instance_clear(mc_container_instance_t *container) {
    if (!container) return;
    for (size_t i = 0; i < MC_CONTAINER_SLOT_COUNT; i++) mc_slot_clear(&container->slots[i]);
    memset(container, 0, sizeof(*container));
}

void mc_inventory_fill_starter_loadout(mc_inventory_t *inv) {
    if (!inv) return;
    static const char *starter_names[MC_PLAYER_HOTBAR_SIZE] = {
        "minecraft:stone",
        "minecraft:dirt",
        "minecraft:grass_block",
        "minecraft:water_bucket",
        "minecraft:lava_bucket",
        "minecraft:flint_and_steel",
        "minecraft:redstone_block",
        "minecraft:redstone",
        "minecraft:redstone_lamp",
    };
    for (int i = 0; i < MC_PLAYER_HOTBAR_SIZE; i++) {
        int32_t item_id = mc_minecraft_item_id(starter_names[i]);
        int32_t count = (strcmp(starter_names[i], "minecraft:water_bucket") == 0 || strcmp(starter_names[i], "minecraft:lava_bucket") == 0 ||
                         strcmp(starter_names[i], "minecraft:flint_and_steel") == 0)
                            ? 1
                            : 64;
        (void)mc_slot_set_simple(&inv->slots[MC_PLAYER_HOTBAR_BASE + i], item_id, count);
    }
    inv->selected_hotbar_slot = 0;
    inv->state_id = 1;
}

int mc_inventory_selected_slot_index(const mc_inventory_t *inv) {
    if (!inv) return -1;
    int32_t hotbar = inv->selected_hotbar_slot;
    if (hotbar < 0 || hotbar >= MC_PLAYER_HOTBAR_SIZE) return -1;
    return MC_PLAYER_HOTBAR_BASE + hotbar;
}

mc_slot_t *mc_inventory_selected_slot(mc_inventory_t *inv) {
    int idx = mc_inventory_selected_slot_index(inv);
    if (!inv || idx < 0) return NULL;
    return &inv->slots[idx];
}

const mc_slot_t *mc_inventory_selected_slot_const(const mc_inventory_t *inv) {
    int idx = mc_inventory_selected_slot_index(inv);
    if (!inv || idx < 0) return NULL;
    return &inv->slots[idx];
}

bool mc_inventory_can_absorb_slot(const mc_inventory_t *inv, const mc_slot_t *src) {
    const int max_stack = 64;
    int remaining;

    if (!inv || !src) return false;
    if (!src->present || src->count <= 0 || src->item_id <= 0) return true;

    remaining = src->count;
    for (int pass = 0; pass < 2 && remaining > 0; pass++) {
        int first = (pass == 0) ? MC_PLAYER_HOTBAR_BASE : 9;
        int last = (pass == 0) ? (MC_PLAYER_HOTBAR_BASE + MC_PLAYER_HOTBAR_SIZE - 1)
                               : (MC_PLAYER_HOTBAR_BASE - 1);
        for (int i = first; i <= last && remaining > 0; i++) {
            const mc_slot_t *dst = &inv->slots[i];
            if (!dst->present || !mc_slot_is_same_item(dst, src)) continue;
            if (dst->count >= max_stack) continue;
            remaining -= max_stack - dst->count;
        }
    }

    for (int pass = 0; pass < 2 && remaining > 0; pass++) {
        int first = (pass == 0) ? MC_PLAYER_HOTBAR_BASE : 9;
        int last = (pass == 0) ? (MC_PLAYER_HOTBAR_BASE + MC_PLAYER_HOTBAR_SIZE - 1)
                               : (MC_PLAYER_HOTBAR_BASE - 1);
        for (int i = first; i <= last && remaining > 0; i++) {
            const mc_slot_t *dst = &inv->slots[i];
            if (dst->present) continue;
            remaining -= max_stack;
        }
    }

    return remaining <= 0;
}

int mc_inventory_try_absorb_slot(mc_inventory_t *inv, mc_slot_t *src) {
    const int max_stack = 64;
    int remaining;
    int absorbed = 0;

    if (!inv || !src) return -1;
    if (!src->present || src->count <= 0 || src->item_id <= 0) return 0;

    remaining = src->count;

    for (int pass = 0; pass < 2 && remaining > 0; pass++) {
        int first = (pass == 0) ? MC_PLAYER_HOTBAR_BASE : 9;
        int last = (pass == 0) ? (MC_PLAYER_HOTBAR_BASE + MC_PLAYER_HOTBAR_SIZE - 1)
                               : (MC_PLAYER_HOTBAR_BASE - 1);
        for (int i = first; i <= last && remaining > 0; i++) {
            mc_slot_t *dst = &inv->slots[i];
            if (!dst->present || !mc_slot_is_same_item(dst, src)) continue;
            if (dst->count >= max_stack) continue;
            int room = max_stack - dst->count;
            int move = room < remaining ? room : remaining;
            dst->count += move;
            remaining -= move;
            absorbed += move;
        }
    }

    for (int pass = 0; pass < 2 && remaining > 0; pass++) {
        int first = (pass == 0) ? MC_PLAYER_HOTBAR_BASE : 9;
        int last = (pass == 0) ? (MC_PLAYER_HOTBAR_BASE + MC_PLAYER_HOTBAR_SIZE - 1)
                               : (MC_PLAYER_HOTBAR_BASE - 1);
        for (int i = first; i <= last && remaining > 0; i++) {
            mc_slot_t *dst = &inv->slots[i];
            if (dst->present) continue;
            int move = remaining > max_stack ? max_stack : remaining;
            if (mc_slot_copy(dst, src) != 0) {
                if (absorbed > 0) break;
                return -1;
            }
            dst->count = move;
            remaining -= move;
            absorbed += move;
        }
    }

    if (absorbed <= 0) return 0;

    if (remaining <= 0) {
        mc_slot_clear(src);
    } else {
        src->count = remaining;
    }
    inv->state_id++;
    return absorbed;
}

void mc_player_data_init(mc_player_data_t *player) {
    if (!player) return;
    memset(player, 0, sizeof(*player));
    mc_inventory_init(&player->inventory);
    player->health = 20.0f;
    player->food_level = 20;
    player->food_saturation = 5.0f;
    player->food_exhaustion = 0.0f;
    player->ender_state_id = 1;
}

void mc_player_data_clear(mc_player_data_t *player) {
    if (!player) return;
    mc_inventory_clear(&player->inventory);
    for (size_t i = 0; i < MC_CONTAINER_SLOT_COUNT; i++) mc_slot_clear(&player->ender_chest[i]);
    memset(player, 0, sizeof(*player));
}

int mc_slot_read_net(const uint8_t *buf, size_t len, size_t *pos, mc_slot_t *out) {
    if (!buf || !pos || !out) return -1;
    mc_slot_t tmp = {0};
    int32_t count = 0;
    if (read_varint_at(buf, len, pos, &count) != 0) return -1;
    if (count <= 0) {
        mc_slot_clear(out);
        return 0;
    }

    int32_t item_id = 0;
    int32_t added = 0;
    int32_t removed = 0;
    if (read_varint_at(buf, len, pos, &item_id) != 0) return -1;
    if (read_varint_at(buf, len, pos, &added) != 0) return -1;
    if (read_varint_at(buf, len, pos, &removed) != 0) return -1;
    if (item_id <= 0) return -1;
    if (added != 0 || removed != 0) return -1;

    tmp.present = true;
    tmp.item_id = item_id;
    tmp.count = count;
    tmp.added_component_count = added;
    tmp.removed_component_count = removed;

    mc_slot_clear(out);
    *out = tmp;
    return 0;
}

int mc_slot_write_net(uint8_t *buf, size_t cap, size_t *pos, const mc_slot_t *slot) {
    if (!buf || !pos) return -1;
    if (!slot || !slot->present || slot->count <= 0 || slot->item_id <= 0) {
        return write_varint_at(buf, cap, pos, 0);
    }
    if (slot->added_component_count != 0 || slot->removed_component_count != 0 || slot->components_len != 0) return -1;
    if (write_varint_at(buf, cap, pos, slot->count) != 0) return -1;
    if (write_varint_at(buf, cap, pos, slot->item_id) != 0) return -1;
    if (write_varint_at(buf, cap, pos, 0) != 0) return -1;
    if (write_varint_at(buf, cap, pos, 0) != 0) return -1;
    return 0;
}
