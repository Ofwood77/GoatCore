#include "mc_player_store.h"

#include "generated_minecraft_ids.h"
#include "mc_nbt.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} byte_buf_t;

static void byte_buf_free(byte_buf_t *b) {
    if (!b) return;
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static int byte_buf_reserve(byte_buf_t *b, size_t need) {
    size_t new_cap;
    uint8_t *next;

    if (!b) return -1;
    if (need <= b->cap) return 0;
    new_cap = b->cap ? b->cap : 4096;
    while (new_cap < need) {
        if (new_cap > SIZE_MAX / 2) return -1;
        new_cap *= 2;
    }
    next = (uint8_t *)realloc(b->data, new_cap);
    if (!next) return -1;
    b->data = next;
    b->cap = new_cap;
    return 0;
}

static int byte_buf_append(byte_buf_t *b, const void *src, size_t n) {
    if (!b || (!src && n != 0)) return -1;
    if (byte_buf_reserve(b, b->len + n) != 0) return -1;
    if (n > 0) memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

static int mkdir_p_local(const char *path, mode_t mode) {
    char tmp[1024];
    size_t n;

    if (!path || !*path) return -1;
    n = strlen(path);
    if (n >= sizeof(tmp)) return -1;
    memcpy(tmp, path, n + 1);

    for (size_t i = 1; i < n; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = '\0';
        if (tmp[0] != '\0' && mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
        tmp[i] = '/';
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
    return 0;
}

static void sanitize_username(const char *src, char *dst, size_t cap) {
    size_t pos = 0;

    if (!dst || cap == 0) return;
    if (src) {
        for (size_t i = 0; src[i] && pos + 1 < cap; i++) {
            char ch = src[i];
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-') {
                dst[pos++] = ch;
            } else {
                dst[pos++] = '_';
            }
        }
    }
    if (pos == 0 && cap > 1) dst[pos++] = 'p';
    dst[pos] = '\0';
}

static int playerdata_dir_path(char *buf, size_t cap, const char *world_path) {
    int n;
    if (!buf || !world_path || !*world_path) return -1;
    n = snprintf(buf, cap, "%s/playerdata", world_path);
    if (n <= 0 || (size_t)n >= cap) return -1;
    return 0;
}

static int player_dat_path(char *buf, size_t cap, const char *world_path, const mc_player_data_t *player) {
    char id_buf[128];
    int n;

    if (!buf || !world_path || !*world_path || !player) return -1;
    if (player->has_uuid) {
        n = snprintf(id_buf, sizeof(id_buf),
                     "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                     player->uuid[0], player->uuid[1], player->uuid[2], player->uuid[3], player->uuid[4], player->uuid[5],
                     player->uuid[6], player->uuid[7], player->uuid[8], player->uuid[9], player->uuid[10], player->uuid[11],
                     player->uuid[12], player->uuid[13], player->uuid[14], player->uuid[15]);
        if (n <= 0 || (size_t)n >= sizeof(id_buf)) return -1;
    } else {
        sanitize_username(player->username, id_buf, sizeof(id_buf));
    }

    n = snprintf(buf, cap, "%s/playerdata/%s.dat", world_path, id_buf);
    if (n <= 0 || (size_t)n >= cap) return -1;
    return 0;
}

static int gzip_write_file(const char *path, const uint8_t *data, size_t len) {
    gzFile gz;
    size_t pos = 0;

    if (!path || !data) return -1;
    gz = gzopen(path, "wb");
    if (!gz) return -1;
    while (pos < len) {
        unsigned chunk = (unsigned)((len - pos) > 1u << 20 ? (1u << 20) : (len - pos));
        int written = gzwrite(gz, data + pos, chunk);
        if (written <= 0 || (unsigned)written != chunk) {
            gzclose(gz);
            return -1;
        }
        pos += (size_t)written;
    }
    return gzclose(gz) == Z_OK ? 0 : -1;
}

static int gzip_read_file(const char *path, uint8_t **out, size_t *out_len) {
    gzFile gz;
    byte_buf_t buf = {0};

    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!path || !out || !out_len) return -1;

    gz = gzopen(path, "rb");
    if (!gz) return -1;

    for (;;) {
        uint8_t tmp[4096];
        int n = gzread(gz, tmp, (unsigned)sizeof(tmp));
        if (n < 0) {
            byte_buf_free(&buf);
            gzclose(gz);
            return -1;
        }
        if (n == 0) break;
        if (byte_buf_append(&buf, tmp, (size_t)n) != 0) {
            byte_buf_free(&buf);
            gzclose(gz);
            return -1;
        }
    }

    if (gzclose(gz) != Z_OK) {
        byte_buf_free(&buf);
        return -1;
    }

    *out = buf.data;
    *out_len = buf.len;
    return 0;
}

static mc_nbt_tag_t *nbt_new(mc_nbt_type_t type, const char *name) {
    mc_nbt_tag_t *tag = (mc_nbt_tag_t *)calloc(1, sizeof(*tag));
    if (!tag) return NULL;
    tag->type = type;
    if (name) {
        tag->name = strdup(name);
        if (!tag->name) {
            free(tag);
            return NULL;
        }
    }
    return tag;
}

static mc_nbt_tag_t *nbt_new_byte(const char *name, int8_t value) {
    mc_nbt_tag_t *tag = nbt_new(MC_NBT_TAG_BYTE, name);
    if (tag) tag->payload.byte_val = value;
    return tag;
}

static mc_nbt_tag_t *nbt_new_int(const char *name, int32_t value) {
    mc_nbt_tag_t *tag = nbt_new(MC_NBT_TAG_INT, name);
    if (tag) tag->payload.int_val = value;
    return tag;
}

static mc_nbt_tag_t *nbt_new_float(const char *name, float value) {
    mc_nbt_tag_t *tag = nbt_new(MC_NBT_TAG_FLOAT, name);
    if (tag) tag->payload.float_val = value;
    return tag;
}

static mc_nbt_tag_t *nbt_new_double(const char *name, double value) {
    mc_nbt_tag_t *tag = nbt_new(MC_NBT_TAG_DOUBLE, name);
    if (tag) tag->payload.double_val = value;
    return tag;
}

static mc_nbt_tag_t *nbt_new_string(const char *name, const char *value) {
    mc_nbt_tag_t *tag = nbt_new(MC_NBT_TAG_STRING, name);
    if (!tag) return NULL;
    tag->payload.string_val = strdup(value ? value : "");
    if (!tag->payload.string_val) {
        mc_nbt_free(tag);
        return NULL;
    }
    return tag;
}

static int compound_add(mc_nbt_tag_t *compound, mc_nbt_tag_t *child) {
    mc_nbt_tag_t **next;
    int32_t len;

    if (!compound || compound->type != MC_NBT_TAG_COMPOUND || !child) return -1;
    len = compound->payload.compound.length;
    next = (mc_nbt_tag_t **)realloc(compound->payload.compound.children, (size_t)(len + 1) * sizeof(*next));
    if (!next) return -1;
    compound->payload.compound.children = next;
    compound->payload.compound.children[len] = child;
    compound->payload.compound.length = len + 1;
    return 0;
}

static int list_add(mc_nbt_tag_t *list, mc_nbt_tag_t *item) {
    mc_nbt_tag_t **next;
    int32_t len;

    if (!list || list->type != MC_NBT_TAG_LIST || !item) return -1;
    if (list->payload.list.length == 0) {
        list->payload.list.elem_type = item->type;
    } else if (list->payload.list.elem_type != item->type) {
        return -1;
    }
    len = list->payload.list.length;
    next = (mc_nbt_tag_t **)realloc(list->payload.list.items, (size_t)(len + 1) * sizeof(*next));
    if (!next) return -1;
    list->payload.list.items = next;
    list->payload.list.items[len] = item;
    list->payload.list.length = len + 1;
    return 0;
}

static int nbt_num_to_i32(const mc_nbt_tag_t *tag, int32_t *out) {
    if (!tag || !out) return -1;
    switch (tag->type) {
        case MC_NBT_TAG_BYTE:
            *out = (int32_t)tag->payload.byte_val;
            return 0;
        case MC_NBT_TAG_SHORT:
            *out = (int32_t)tag->payload.short_val;
            return 0;
        case MC_NBT_TAG_INT:
            *out = tag->payload.int_val;
            return 0;
        default:
            return -1;
    }
}

static int nbt_num_to_f32(const mc_nbt_tag_t *tag, float *out) {
    if (!tag || !out) return -1;
    switch (tag->type) {
        case MC_NBT_TAG_FLOAT:
            *out = tag->payload.float_val;
            return 0;
        case MC_NBT_TAG_DOUBLE:
            *out = (float)tag->payload.double_val;
            return 0;
        default:
            return -1;
    }
}

static int nbt_num_to_f64(const mc_nbt_tag_t *tag, double *out) {
    if (!tag || !out) return -1;
    switch (tag->type) {
        case MC_NBT_TAG_DOUBLE:
            *out = tag->payload.double_val;
            return 0;
        case MC_NBT_TAG_FLOAT:
            *out = (double)tag->payload.float_val;
            return 0;
        default:
            return -1;
    }
}

static mc_nbt_tag_t *make_slot_compound(int32_t slot_index, const mc_slot_t *slot) {
    const char *item_name;
    mc_nbt_tag_t *entry = NULL;

    if (!slot || !slot->present || slot->count <= 0 || slot->item_id <= 0) return NULL;
    item_name = mc_minecraft_item_name(slot->item_id);
    if (!item_name || !*item_name) return NULL;

    entry = nbt_new(MC_NBT_TAG_COMPOUND, NULL);
    if (!entry) return NULL;
    if (compound_add(entry, nbt_new_byte("slot", (int8_t)slot_index)) != 0 ||
        compound_add(entry, nbt_new_string("id", item_name)) != 0 ||
        compound_add(entry, nbt_new_byte("count", (int8_t)slot->count)) != 0) {
        mc_nbt_free(entry);
        return NULL;
    }
    return entry;
}

static int build_inventory_list(mc_nbt_tag_t *list, const mc_slot_t *slots, int slot_count) {
    if (!list || list->type != MC_NBT_TAG_LIST || !slots || slot_count < 0) return -1;
    list->payload.list.elem_type = MC_NBT_TAG_COMPOUND;
    for (int i = 0; i < slot_count; i++) {
        mc_nbt_tag_t *entry = make_slot_compound(i, &slots[i]);
        if (!entry) continue;
        if (list_add(list, entry) != 0) {
            mc_nbt_free(entry);
            return -1;
        }
    }
    return 0;
}

static int build_single_slot_compound(mc_nbt_tag_t *compound, const char *name, const mc_slot_t *slot) {
    mc_nbt_tag_t *entry;

    if (!compound || compound->type != MC_NBT_TAG_COMPOUND || !name) return -1;
    if (!slot || !slot->present || slot->count <= 0 || slot->item_id <= 0) return 0;

    entry = make_slot_compound(0, slot);
    if (!entry) return -1;
    free(entry->name);
    entry->name = strdup(name);
    if (!entry->name) {
        mc_nbt_free(entry);
        return -1;
    }
    return compound_add(compound, entry);
}

static int parse_inventory_list(const mc_nbt_tag_t *list, mc_slot_t *slots, int slot_count) {
    if (!list || !slots || slot_count < 0) return -1;
    if (list->type != MC_NBT_TAG_LIST || list->payload.list.elem_type != MC_NBT_TAG_COMPOUND) return -1;

    for (int32_t i = 0; i < list->payload.list.length; i++) {
        const mc_nbt_tag_t *entry = list->payload.list.items ? list->payload.list.items[i] : NULL;
        const mc_nbt_tag_t *slot_tag;
        const mc_nbt_tag_t *id_tag;
        const mc_nbt_tag_t *count_tag;
        int32_t slot_index = -1;
        int32_t count = 0;
        int32_t item_id = -1;

        if (!entry || entry->type != MC_NBT_TAG_COMPOUND) continue;
        slot_tag = mc_nbt_compound_get(entry, "slot");
        if (!slot_tag) slot_tag = mc_nbt_compound_get(entry, "Slot");
        id_tag = mc_nbt_compound_get(entry, "id");
        count_tag = mc_nbt_compound_get(entry, "count");
        if (!count_tag) count_tag = mc_nbt_compound_get(entry, "Count");
        if (nbt_num_to_i32(slot_tag, &slot_index) != 0 || slot_index < 0 || slot_index >= slot_count) continue;
        if (!id_tag || id_tag->type != MC_NBT_TAG_STRING) continue;
        if (nbt_num_to_i32(count_tag, &count) != 0 || count <= 0) continue;
        item_id = mc_minecraft_item_id(id_tag->payload.string_val);
        if (item_id <= 0) continue;
        (void)mc_slot_set_simple(&slots[slot_index], item_id, count);
    }

    return 0;
}

static int parse_single_slot_compound(const mc_nbt_tag_t *entry, mc_slot_t *slot) {
    const mc_nbt_tag_t *id_tag;
    const mc_nbt_tag_t *count_tag;
    int32_t count = 0;
    int32_t item_id = -1;

    if (!entry || !slot || entry->type != MC_NBT_TAG_COMPOUND) return -1;
    id_tag = mc_nbt_compound_get(entry, "id");
    count_tag = mc_nbt_compound_get(entry, "count");
    if (!count_tag) count_tag = mc_nbt_compound_get(entry, "Count");
    if (!id_tag || id_tag->type != MC_NBT_TAG_STRING) return -1;
    if (nbt_num_to_i32(count_tag, &count) != 0 || count <= 0) return -1;
    item_id = mc_minecraft_item_id(id_tag->payload.string_val);
    if (item_id <= 0) return -1;
    return mc_slot_set_simple(slot, item_id, count);
}

static int player_build_nbt(const mc_player_data_t *player, mc_nbt_tag_t **out_root) {
    mc_nbt_tag_t *root = NULL;
    mc_nbt_tag_t *pos = NULL;
    mc_nbt_tag_t *rot = NULL;
    mc_nbt_tag_t *inv = NULL;
    mc_nbt_tag_t *ender = NULL;

    if (out_root) *out_root = NULL;
    if (!player || !out_root) return -1;

    root = nbt_new(MC_NBT_TAG_COMPOUND, "");
    pos = nbt_new(MC_NBT_TAG_LIST, "Pos");
    rot = nbt_new(MC_NBT_TAG_LIST, "Rotation");
    inv = nbt_new(MC_NBT_TAG_LIST, "Inventory");
    ender = nbt_new(MC_NBT_TAG_LIST, "EnderItems");
    if (!root || !pos || !rot || !inv || !ender) goto fail;

    pos->payload.list.elem_type = MC_NBT_TAG_DOUBLE;
    rot->payload.list.elem_type = MC_NBT_TAG_FLOAT;
    inv->payload.list.elem_type = MC_NBT_TAG_COMPOUND;
    ender->payload.list.elem_type = MC_NBT_TAG_COMPOUND;

    if (list_add(pos, nbt_new_double(NULL, player->pos_x)) != 0 ||
        list_add(pos, nbt_new_double(NULL, player->pos_y)) != 0 ||
        list_add(pos, nbt_new_double(NULL, player->pos_z)) != 0 ||
        list_add(rot, nbt_new_float(NULL, player->yaw)) != 0 ||
        list_add(rot, nbt_new_float(NULL, player->pitch)) != 0 ||
        build_inventory_list(inv, player->inventory.slots, MC_PLAYER_SLOT_COUNT) != 0 ||
        build_inventory_list(ender, player->ender_chest, MC_CONTAINER_SLOT_COUNT) != 0) {
        goto fail;
    }

    if (compound_add(root, pos) != 0 ||
        compound_add(root, rot) != 0 ||
        compound_add(root, inv) != 0 ||
        compound_add(root, ender) != 0 ||
        compound_add(root, nbt_new_int("playerGameType", player->gamemode)) != 0 ||
        compound_add(root, nbt_new_float("Health", player->health)) != 0 ||
        compound_add(root, nbt_new_int("foodLevel", player->food_level)) != 0 ||
        compound_add(root, nbt_new_float("foodSaturationLevel", player->food_saturation)) != 0 ||
        compound_add(root, nbt_new_float("foodExhaustionLevel", player->food_exhaustion)) != 0 ||
        compound_add(root, nbt_new_int("SelectedItemSlot", player->inventory.selected_hotbar_slot)) != 0 ||
        compound_add(root, nbt_new_int("mcInventoryStateId", player->inventory.state_id)) != 0 ||
        compound_add(root, nbt_new_int("mcEnderStateId", player->ender_state_id)) != 0 ||
        build_single_slot_compound(root, "mcCursorItem", &player->inventory.cursor_slot) != 0) {
        goto fail;
    }

    pos = NULL;
    rot = NULL;
    inv = NULL;
    ender = NULL;
    *out_root = root;
    return 0;

fail:
    mc_nbt_free(pos);
    mc_nbt_free(rot);
    mc_nbt_free(inv);
    mc_nbt_free(ender);
    mc_nbt_free(root);
    return -1;
}

static int player_parse_nbt(const mc_nbt_tag_t *root, mc_player_data_t *out) {
    const mc_nbt_tag_t *pos;
    const mc_nbt_tag_t *rot;
    const mc_nbt_tag_t *gamemode;
    const mc_nbt_tag_t *health;
    const mc_nbt_tag_t *food_level;
    const mc_nbt_tag_t *food_saturation;
    const mc_nbt_tag_t *food_exhaustion;
    const mc_nbt_tag_t *selected;
    const mc_nbt_tag_t *inventory_state;
    const mc_nbt_tag_t *ender_state;
    const mc_nbt_tag_t *cursor_item;
    const mc_nbt_tag_t *inv;
    const mc_nbt_tag_t *ender;
    double px = 0.5;
    double py = 64.0;
    double pz = 0.5;
    float yaw = 0.0f;
    float pitch = 0.0f;

    if (!root || !out || root->type != MC_NBT_TAG_COMPOUND) return -1;

    pos = mc_nbt_compound_get(root, "Pos");
    if (pos && pos->type == MC_NBT_TAG_LIST && pos->payload.list.length >= 3) {
        (void)nbt_num_to_f64(pos->payload.list.items[0], &px);
        (void)nbt_num_to_f64(pos->payload.list.items[1], &py);
        (void)nbt_num_to_f64(pos->payload.list.items[2], &pz);
    }

    rot = mc_nbt_compound_get(root, "Rotation");
    if (rot && rot->type == MC_NBT_TAG_LIST && rot->payload.list.length >= 2) {
        (void)nbt_num_to_f32(rot->payload.list.items[0], &yaw);
        (void)nbt_num_to_f32(rot->payload.list.items[1], &pitch);
    }

    out->pos_x = px;
    out->pos_y = py;
    out->pos_z = pz;
    out->yaw = yaw;
    out->pitch = pitch;

    gamemode = mc_nbt_compound_get(root, "playerGameType");
    if (gamemode) (void)nbt_num_to_i32(gamemode, &out->gamemode);

    health = mc_nbt_compound_get(root, "Health");
    if (health) (void)nbt_num_to_f32(health, &out->health);

    food_level = mc_nbt_compound_get(root, "foodLevel");
    if (food_level) (void)nbt_num_to_i32(food_level, &out->food_level);

    food_saturation = mc_nbt_compound_get(root, "foodSaturationLevel");
    if (food_saturation) (void)nbt_num_to_f32(food_saturation, &out->food_saturation);

    food_exhaustion = mc_nbt_compound_get(root, "foodExhaustionLevel");
    if (food_exhaustion) (void)nbt_num_to_f32(food_exhaustion, &out->food_exhaustion);

    selected = mc_nbt_compound_get(root, "SelectedItemSlot");
    if (selected) (void)nbt_num_to_i32(selected, &out->inventory.selected_hotbar_slot);

    inventory_state = mc_nbt_compound_get(root, "mcInventoryStateId");
    if (inventory_state) (void)nbt_num_to_i32(inventory_state, &out->inventory.state_id);

    ender_state = mc_nbt_compound_get(root, "mcEnderStateId");
    if (ender_state) (void)nbt_num_to_i32(ender_state, &out->ender_state_id);

    inv = mc_nbt_compound_get(root, "Inventory");
    if (inv) (void)parse_inventory_list(inv, out->inventory.slots, MC_PLAYER_SLOT_COUNT);

    cursor_item = mc_nbt_compound_get(root, "mcCursorItem");
    if (cursor_item) (void)parse_single_slot_compound(cursor_item, &out->inventory.cursor_slot);

    ender = mc_nbt_compound_get(root, "EnderItems");
    if (ender) (void)parse_inventory_list(ender, out->ender_chest, MC_CONTAINER_SLOT_COUNT);

    return 0;
}

int mc_player_store_save_to_file(const char *world_path, const mc_player_data_t *player) {
    char dir_path[1024];
    char file_path[1024];
    mc_nbt_tag_t *root = NULL;
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    int rc = -1;

    if (!world_path || !*world_path || !player) return -1;
    if (playerdata_dir_path(dir_path, sizeof(dir_path), world_path) != 0) return -1;
    if (player_dat_path(file_path, sizeof(file_path), world_path, player) != 0) return -1;
    if (mkdir_p_local(dir_path, 0755) != 0) return -1;

    if (player_build_nbt(player, &root) != 0) goto cleanup;
    if (mc_nbt_write_named_root(root, &raw, &raw_len) != 0) goto cleanup;
    if (gzip_write_file(file_path, raw, raw_len) != 0) goto cleanup;
    rc = 0;

cleanup:
    free(raw);
    mc_nbt_free(root);
    return rc;
}

int mc_player_store_save(const char *world_path, const mc_player_data_t *player) {
    return mc_player_store_save_to_file(world_path, player);
}

int mc_player_store_load(const char *world_path, const uint8_t uuid[16], bool has_uuid, const char *username, mc_player_data_t *out) {
    mc_player_data_t key = {0};
    char file_path[1024];
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    mc_nbt_tag_t *root = NULL;
    mc_player_data_t tmp;
    int rc = 1;

    if (!world_path || !*world_path || !out) return 1;

    key.has_uuid = has_uuid;
    if (uuid) memcpy(key.uuid, uuid, sizeof(key.uuid));
    if (username) snprintf(key.username, sizeof(key.username), "%s", username);
    if (player_dat_path(file_path, sizeof(file_path), world_path, &key) != 0) return -1;
    if (access(file_path, R_OK) != 0) return (errno == ENOENT) ? 1 : -1;

    if (gzip_read_file(file_path, &raw, &raw_len) != 0) return -1;
    if (mc_nbt_read_named_root(raw, raw_len, &root, NULL) != 0) {
        free(raw);
        return -1;
    }

    mc_player_data_init(&tmp);
    tmp.has_uuid = has_uuid;
    if (uuid) memcpy(tmp.uuid, uuid, sizeof(tmp.uuid));
    if (username) snprintf(tmp.username, sizeof(tmp.username), "%s", username);
    tmp.gamemode = 1;
    tmp.health = 20.0f;
    tmp.food_level = 20;
    tmp.food_saturation = 5.0f;
    tmp.pos_x = 0.5;
    tmp.pos_y = 64.0;
    tmp.pos_z = 0.5;
    tmp.yaw = 0.0f;
    tmp.pitch = 0.0f;
    tmp.ender_state_id = 1;

    if (player_parse_nbt(root, &tmp) != 0) {
        mc_player_data_clear(&tmp);
        mc_nbt_free(root);
        free(raw);
        return -1;
    }

    mc_player_data_clear(out);
    *out = tmp;
    rc = 0;

    mc_nbt_free(root);
    free(raw);
    return rc;
}
