#include "mc_player_store.h"

#include "generated_minecraft_ids.h"
#include "mc_nbt.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
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

/* Player gameplay is tick-owned, but this also serializes direct store users. */
static pthread_mutex_t player_store_mutex = PTHREAD_MUTEX_INITIALIZER;

#define MC_PLAYER_NBT_MAX_BYTES (8u * 1024u * 1024u)
#define MC_PLAYER_DATA_VERSION_26_1_1 4788

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
    n = snprintf(buf, cap, "%s/players/data", world_path);
    if (n <= 0 || (size_t)n >= cap) return -1;
    return 0;
}

static int player_dat_path(char *buf, size_t cap, const char *world_path, const mc_player_data_t *player,
                           bool legacy_path) {
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

    n = snprintf(buf, cap, legacy_path ? "%s/playerdata/%s.dat" : "%s/players/data/%s.dat", world_path, id_buf);
    if (n <= 0 || (size_t)n >= cap) return -1;
    return 0;
}

static int gzip_write_file_atomic(const char *path, const char *dir_path, const uint8_t *data, size_t len) {
    char temporary[1100];
    gzFile gz;
    size_t pos = 0;
    int fd = -1;
    int sync_fd = -1;
    int dir_fd = -1;
    int rc = -1;

    if (!path || !dir_path || !data) return -1;
    if (snprintf(temporary, sizeof(temporary), "%s/.player-XXXXXX", dir_path) >= (int)sizeof(temporary)) return -1;
    fd = mkstemp(temporary);
    if (fd < 0) return -1;
    if (fchmod(fd, 0644) != 0) goto cleanup;
    gz = gzdopen(fd, "wb");
    if (!gz) goto cleanup;
    fd = -1; /* gzclose owns this descriptor. */
    while (pos < len) {
        unsigned chunk = (unsigned)((len - pos) > 1u << 20 ? (1u << 20) : (len - pos));
        int written = gzwrite(gz, data + pos, chunk);
        if (written <= 0 || (unsigned)written != chunk) {
            gzclose(gz);
            goto cleanup;
        }
        pos += (size_t)written;
    }
    if (gzclose(gz) != Z_OK) goto cleanup;
    sync_fd = open(temporary, O_RDONLY | O_CLOEXEC);
    if (sync_fd < 0 || fsync(sync_fd) != 0) goto cleanup;
    if (close(sync_fd) != 0) { sync_fd = -1; goto cleanup; }
    sync_fd = -1;
    if (rename(temporary, path) != 0) goto cleanup;
    dir_fd = open(dir_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0 || fsync(dir_fd) != 0) goto cleanup;
    rc = 0;
cleanup:
    if (fd >= 0) close(fd);
    if (sync_fd >= 0) close(sync_fd);
    if (dir_fd >= 0) close(dir_fd);
    if (rc != 0) unlink(temporary);
    return rc;
}

static int sync_directory(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    if (close(fd) != 0) rc = -1;
    return rc;
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
        if (buf.len + (size_t)n > MC_PLAYER_NBT_MAX_BYTES) {
            byte_buf_free(&buf);
            gzclose(gz);
            return -1;
        }
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

static mc_nbt_tag_t *nbt_new_uuid(const uint8_t uuid[16]) {
    mc_nbt_tag_t *tag = nbt_new(MC_NBT_TAG_INT_ARRAY, "UUID");
    if (!tag) return NULL;
    tag->payload.int_array.data = malloc(4 * sizeof(int32_t));
    if (!tag->payload.int_array.data) { mc_nbt_free(tag); return NULL; }
    tag->payload.int_array.length = 4;
    for (int i = 0; i < 4; i++) {
        uint32_t bits = ((uint32_t)uuid[i * 4] << 24) | ((uint32_t)uuid[i * 4 + 1] << 16) |
                        ((uint32_t)uuid[i * 4 + 2] << 8) | uuid[i * 4 + 3];
        memcpy(&tag->payload.int_array.data[i], &bits, sizeof(bits));
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

static mc_nbt_tag_t *make_slot_compound(int32_t slot_index, const mc_slot_t *slot, const char *name,
                                        bool with_slot) {
    const char *item_name;
    mc_nbt_tag_t *entry = NULL;

    if (!slot || !slot->present || slot->count <= 0 || slot->count > 99 || slot->item_id <= 0 ||
        slot->damage != 0 || slot->added_component_count != 0 || slot->removed_component_count != 0 ||
        slot->components_len != 0) return NULL;
    item_name = mc_minecraft_item_name(slot->item_id);
    if (!item_name || !*item_name) return NULL;

    entry = nbt_new(MC_NBT_TAG_COMPOUND, name);
    if (!entry) return NULL;
    if ((with_slot && compound_add(entry, nbt_new_byte("Slot", (int8_t)slot_index)) != 0) ||
        compound_add(entry, nbt_new_string("id", item_name)) != 0 ||
        compound_add(entry, nbt_new_int("count", slot->count)) != 0) {
        mc_nbt_free(entry);
        return NULL;
    }
    return entry;
}

static int disk_slot_from_network(int slot) {
    if (slot >= 36 && slot <= 44) return slot - 36; /* hotbar */
    if (slot >= 9 && slot <= 35) return slot;
    return -1; /* equipment and transient crafting slots are separate */
}

static int network_slot_from_disk(int slot, int format) {
    if (format == 1) return slot >= 0 && slot < MC_PLAYER_SLOT_COUNT ? slot : -1;
    if (slot >= 0 && slot <= 8) return slot + 36;
    if (slot >= 9 && slot <= 35) return slot;
    if (format == 2 && slot >= 100 && slot <= 103) return 5 + (103 - slot);
    if (format == 2 && slot == -106) return 45;
    return -1;
}

static int build_inventory_list(mc_nbt_tag_t *list, const mc_slot_t *slots, int slot_count, bool player_inventory) {
    if (!list || list->type != MC_NBT_TAG_LIST || !slots || slot_count < 0) return -1;
    list->payload.list.elem_type = MC_NBT_TAG_COMPOUND;
    for (int i = 0; i < slot_count; i++) {
        int disk_slot = player_inventory ? disk_slot_from_network(i) : i;
        mc_nbt_tag_t *entry;
        if (disk_slot == -1) continue;
        if (!slots[i].present) continue;
        entry = make_slot_compound(disk_slot, &slots[i], NULL, true);
        if (!entry) return -1;
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
    if (!slot || !slot->present) return 0;

    entry = make_slot_compound(0, slot, name, false);
    if (!entry) return -1;
    return compound_add(compound, entry);
}

static int build_crafting_items(mc_nbt_tag_t *root, const mc_inventory_t *inventory) {
    mc_nbt_tag_t *list = nbt_new(MC_NBT_TAG_LIST, "mcCraftingItems");
    if (!list) return -1;
    /* Slot zero is a derived result, never a second owned stack. */
    if (build_inventory_list(list, &inventory->slots[1], 4, false) != 0 ||
        compound_add(root, list) != 0) {
        mc_nbt_free(list);
        return -1;
    }
    return 0;
}

static int build_equipment(mc_nbt_tag_t *root, const mc_inventory_t *inv) {
    static const struct { int index; const char *name; } slots[] = {
        {5, "head"}, {6, "chest"}, {7, "legs"}, {8, "feet"}, {45, "offhand"},
    };
    mc_nbt_tag_t *equipment = nbt_new(MC_NBT_TAG_COMPOUND, "equipment");
    if (!equipment) return -1;
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        const mc_slot_t *slot = &inv->slots[slots[i].index];
        mc_nbt_tag_t *item;
        if (!slot->present) continue;
        item = make_slot_compound(0, slot, slots[i].name, false);
        if (!item || compound_add(equipment, item) != 0) {
            mc_nbt_free(item);
            mc_nbt_free(equipment);
            return -1;
        }
    }
    if (compound_add(root, equipment) != 0) { mc_nbt_free(equipment); return -1; }
    return 0;
}

static int parse_inventory_list(const mc_nbt_tag_t *list, mc_slot_t *slots, int slot_count, bool player_inventory,
                                int format) {
    if (!list || !slots || slot_count < 0) return -1;
    if (list->type != MC_NBT_TAG_LIST ||
        (list->payload.list.elem_type != MC_NBT_TAG_COMPOUND &&
         !(list->payload.list.length == 0 && list->payload.list.elem_type == MC_NBT_TAG_END))) return -1;

    for (int32_t i = 0; i < list->payload.list.length; i++) {
        const mc_nbt_tag_t *entry = list->payload.list.items ? list->payload.list.items[i] : NULL;
        const mc_nbt_tag_t *slot_tag;
        const mc_nbt_tag_t *id_tag;
        const mc_nbt_tag_t *count_tag;
        int32_t slot_index = -1;
        int32_t count = 0;
        int32_t item_id = -1;

        if (!entry || entry->type != MC_NBT_TAG_COMPOUND) return -1;
        for (int32_t j = 0; j < entry->payload.compound.length; j++) {
            const char *name = entry->payload.compound.children[j]->name;
            if (!name || (strcmp(name, "slot") != 0 && strcmp(name, "Slot") != 0 &&
                          strcmp(name, "id") != 0 && strcmp(name, "count") != 0 &&
                          strcmp(name, "Count") != 0 && strcmp(name, "components") != 0)) return -1;
        }
        slot_tag = mc_nbt_compound_get(entry, "slot");
        if (!slot_tag) slot_tag = mc_nbt_compound_get(entry, "Slot");
        id_tag = mc_nbt_compound_get(entry, "id");
        count_tag = mc_nbt_compound_get(entry, "count");
        if (!count_tag) count_tag = mc_nbt_compound_get(entry, "Count");
        if (nbt_num_to_i32(slot_tag, &slot_index) != 0) return -1;
        if (player_inventory) slot_index = network_slot_from_disk(slot_index, format);
        if (slot_index < 0 || slot_index >= slot_count) return -1;
        if (!id_tag || id_tag->type != MC_NBT_TAG_STRING) return -1;
        if (nbt_num_to_i32(count_tag, &count) != 0 || count <= 0 || count > 99) return -1;
        /* Keep a valid file untouched when this runtime cannot represent an
         * item's data components, instead of silently stripping them. */
        const mc_nbt_tag_t *components = mc_nbt_compound_get(entry, "components");
        if (components && (components->type != MC_NBT_TAG_COMPOUND || components->payload.compound.length != 0)) return -1;
        item_id = mc_minecraft_item_id(id_tag->payload.string_val);
        if (item_id <= 0 || slots[slot_index].present) return -1;
        if (mc_slot_set_simple(&slots[slot_index], item_id, count) != 0) return -1;
    }

    return 0;
}

static int parse_single_slot_compound(const mc_nbt_tag_t *entry, mc_slot_t *slot) {
    const mc_nbt_tag_t *id_tag;
    const mc_nbt_tag_t *count_tag;
    int32_t count = 0;
    int32_t item_id = -1;

    if (!entry || !slot || entry->type != MC_NBT_TAG_COMPOUND) return -1;
    for (int32_t j = 0; j < entry->payload.compound.length; j++) {
        const char *name = entry->payload.compound.children[j]->name;
        if (!name || (strcmp(name, "slot") != 0 && strcmp(name, "Slot") != 0 &&
                      strcmp(name, "id") != 0 && strcmp(name, "count") != 0 &&
                      strcmp(name, "Count") != 0 && strcmp(name, "components") != 0)) return -1;
    }
    id_tag = mc_nbt_compound_get(entry, "id");
    count_tag = mc_nbt_compound_get(entry, "count");
    if (!count_tag) count_tag = mc_nbt_compound_get(entry, "Count");
    if (!id_tag || id_tag->type != MC_NBT_TAG_STRING) return -1;
    if (nbt_num_to_i32(count_tag, &count) != 0 || count <= 0 || count > 99) return -1;
    const mc_nbt_tag_t *components = mc_nbt_compound_get(entry, "components");
    if (components && (components->type != MC_NBT_TAG_COMPOUND || components->payload.compound.length != 0)) return -1;
    item_id = mc_minecraft_item_id(id_tag->payload.string_val);
    if (item_id <= 0) return -1;
    return mc_slot_set_simple(slot, item_id, count);
}

static int parse_equipment(const mc_nbt_tag_t *equipment, mc_inventory_t *inventory) {
    static const struct { int index; const char *name; } slots[] = {
        {5, "head"}, {6, "chest"}, {7, "legs"}, {8, "feet"}, {45, "offhand"},
    };
    if (!equipment) return 0;
    if (equipment->type != MC_NBT_TAG_COMPOUND) return -1;
    for (int32_t i = 0; i < equipment->payload.compound.length; i++) {
        const mc_nbt_tag_t *entry = equipment->payload.compound.children[i];
        bool matched = false;
        if (!entry || !entry->name) return -1;
        for (size_t j = 0; j < sizeof(slots) / sizeof(slots[0]); j++) {
            if (strcmp(entry->name, slots[j].name) != 0) continue;
            matched = true;
            if (inventory->slots[slots[j].index].present ||
                parse_single_slot_compound(entry, &inventory->slots[slots[j].index]) != 0) return -1;
            break;
        }
        if (!matched) return -1;
    }
    return 0;
}

static bool player_tag_rebuilt(const char *name) {
    static const char *const names[] = {
        "Pos", "Rotation", "Inventory", "EnderItems", "equipment", "Dimension", "DataVersion", "UUID",
        "playerGameType", "Health", "foodLevel", "foodSaturationLevel", "foodExhaustionLevel",
        "XpP", "XpLevel", "XpTotal", "SelectedItemSlot", "mcInventoryStateId",
        "mcInventoryFormat", "mcEnderStateId", "mcCursorItem", "mcCraftingItems", "abilities",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(name, names[i]) == 0) return true;
    }
    return false;
}

static mc_nbt_tag_t *clone_named_tag(const mc_nbt_tag_t *tag) {
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    size_t read_len = 0;
    mc_nbt_tag_t *copy = NULL;
    if (!tag || !tag->name || mc_nbt_write_named_root(tag, &raw, &raw_len) != 0) return NULL;
    if (mc_nbt_read_named_root(raw, raw_len, &copy, &read_len) != 0 || read_len != raw_len) {
        mc_nbt_free(copy);
        copy = NULL;
    }
    free(raw);
    return copy;
}

static int copy_unhandled_player_tags(mc_nbt_tag_t *dst, const mc_nbt_tag_t *src) {
    if (!src) return 0;
    if (src->type != MC_NBT_TAG_COMPOUND) return -1;
    for (int32_t i = 0; i < src->payload.compound.length; i++) {
        const mc_nbt_tag_t *child = src->payload.compound.children[i];
        mc_nbt_tag_t *copy;
        if (!child || !child->name) return -1;
        if (player_tag_rebuilt(child->name)) continue;
        copy = clone_named_tag(child);
        if (!copy) return -1;
        if (compound_add(dst, copy) != 0) { mc_nbt_free(copy); return -1; }
    }
    return 0;
}

static int set_compound_byte(mc_nbt_tag_t *compound, const char *name, int8_t value) {
    mc_nbt_tag_t *tag = (mc_nbt_tag_t *)mc_nbt_compound_get(compound, name);
    if (tag) {
        if (tag->type != MC_NBT_TAG_BYTE) return -1;
        tag->payload.byte_val = value;
        return 0;
    }
    return compound_add(compound, nbt_new_byte(name, value));
}

static int build_abilities(mc_nbt_tag_t *root, const mc_player_data_t *player) {
    const mc_nbt_tag_t *old = player->passthrough_nbt ?
        mc_nbt_compound_get(player->passthrough_nbt, "abilities") : NULL;
    mc_nbt_tag_t *abilities = old ? clone_named_tag(old) : nbt_new(MC_NBT_TAG_COMPOUND, "abilities");
    if (!abilities || abilities->type != MC_NBT_TAG_COMPOUND) { mc_nbt_free(abilities); return -1; }
    bool creative = player->gamemode == 1;
    bool spectator = player->gamemode == 3;
    if (set_compound_byte(abilities, "invulnerable", creative || spectator) != 0 ||
        set_compound_byte(abilities, "mayfly", creative || spectator) != 0 ||
        set_compound_byte(abilities, "instabuild", creative) != 0 ||
        set_compound_byte(abilities, "flying", (creative || spectator) && player->flying) != 0 ||
        set_compound_byte(abilities, "mayBuild", player->gamemode == 0 || creative) != 0) {
        mc_nbt_free(abilities);
        return -1;
    }
    if (!mc_nbt_compound_get(abilities, "flySpeed") &&
        compound_add(abilities, nbt_new_float("flySpeed", 0.05f)) != 0) { mc_nbt_free(abilities); return -1; }
    if (!mc_nbt_compound_get(abilities, "walkSpeed") &&
        compound_add(abilities, nbt_new_float("walkSpeed", 0.1f)) != 0) { mc_nbt_free(abilities); return -1; }
    if (compound_add(root, abilities) != 0) { mc_nbt_free(abilities); return -1; }
    return 0;
}

static int player_build_nbt(const mc_player_data_t *player, mc_nbt_tag_t **out_root) {
    mc_nbt_tag_t *root = NULL;
    mc_nbt_tag_t *pos = NULL;
    mc_nbt_tag_t *rot = NULL;
    mc_nbt_tag_t *inv = NULL;
    mc_nbt_tag_t *ender = NULL;

    if (out_root) *out_root = NULL;
    if (!player || !out_root) return -1;
    if (!isfinite(player->pos_x) || !isfinite(player->pos_y) || !isfinite(player->pos_z) ||
        !isfinite(player->yaw) || !isfinite(player->pitch) || !isfinite(player->health) ||
        !isfinite(player->food_saturation) || !isfinite(player->food_exhaustion) ||
        !isfinite(player->experience_progress) || player->gamemode < 0 || player->gamemode > 3 ||
        player->food_level < 0 || player->food_level > 20 ||
        player->inventory.selected_hotbar_slot < 0 ||
        player->inventory.selected_hotbar_slot >= MC_PLAYER_HOTBAR_SIZE) return -1;

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
        build_inventory_list(inv, player->inventory.slots, MC_PLAYER_SLOT_COUNT, true) != 0 ||
        build_inventory_list(ender, player->ender_chest, MC_CONTAINER_SLOT_COUNT, false) != 0) {
        goto fail;
    }

    if (compound_add(root, pos) != 0) goto fail;
    pos = NULL;
    if (compound_add(root, rot) != 0) goto fail;
    rot = NULL;
    if (compound_add(root, inv) != 0) goto fail;
    inv = NULL;
    if (compound_add(root, ender) != 0) goto fail;
    ender = NULL;

    if ((player->has_uuid && compound_add(root, nbt_new_uuid(player->uuid)) != 0) ||
        compound_add(root, nbt_new_string("Dimension", "minecraft:overworld")) != 0 ||
        compound_add(root, nbt_new_int("DataVersion", MC_PLAYER_DATA_VERSION_26_1_1)) != 0 ||
        compound_add(root, nbt_new_int("playerGameType", player->gamemode)) != 0 ||
        compound_add(root, nbt_new_float("Health", player->health)) != 0 ||
        compound_add(root, nbt_new_int("foodLevel", player->food_level)) != 0 ||
        compound_add(root, nbt_new_float("foodSaturationLevel", player->food_saturation)) != 0 ||
        compound_add(root, nbt_new_float("foodExhaustionLevel", player->food_exhaustion)) != 0 ||
        compound_add(root, nbt_new_float("XpP", player->experience_progress)) != 0 ||
        compound_add(root, nbt_new_int("XpLevel", player->experience_level)) != 0 ||
        compound_add(root, nbt_new_int("XpTotal", player->experience_total)) != 0 ||
        compound_add(root, nbt_new_int("SelectedItemSlot", player->inventory.selected_hotbar_slot)) != 0 ||
        compound_add(root, nbt_new_int("mcInventoryStateId", player->inventory.state_id)) != 0 ||
        compound_add(root, nbt_new_int("mcInventoryFormat", 3)) != 0 ||
        compound_add(root, nbt_new_int("mcEnderStateId", player->ender_state_id)) != 0 ||
        build_single_slot_compound(root, "mcCursorItem", &player->inventory.cursor_slot) != 0 ||
        build_crafting_items(root, &player->inventory) != 0 ||
        build_equipment(root, &player->inventory) != 0 ||
        build_abilities(root, player) != 0 ||
        copy_unhandled_player_tags(root, player->passthrough_nbt) != 0) {
        goto fail;
    }

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
    const mc_nbt_tag_t *xp_progress;
    const mc_nbt_tag_t *xp_level;
    const mc_nbt_tag_t *xp_total;
    const mc_nbt_tag_t *selected;
    const mc_nbt_tag_t *inventory_state;
    const mc_nbt_tag_t *ender_state;
    const mc_nbt_tag_t *cursor_item;
    const mc_nbt_tag_t *inv;
    const mc_nbt_tag_t *ender;
    const mc_nbt_tag_t *equipment;
    const mc_nbt_tag_t *format;
    const mc_nbt_tag_t *dimension;
    const mc_nbt_tag_t *abilities;
    const mc_nbt_tag_t *crafting;
    int32_t format_version = 3;
    double px = 0.5;
    double py = 64.0;
    double pz = 0.5;
    float yaw = 0.0f;
    float pitch = 0.0f;

    if (!root || !out || root->type != MC_NBT_TAG_COMPOUND) return -1;

    const mc_nbt_tag_t *identity = mc_nbt_compound_get(root, "UUID");
    if (identity) {
        if (identity->type != MC_NBT_TAG_INT_ARRAY || identity->payload.int_array.length != 4) return -1;
        uint8_t uuid[16];
        for (int i = 0; i < 4; i++) {
            uint32_t bits = (uint32_t)identity->payload.int_array.data[i];
            uuid[i * 4] = (uint8_t)(bits >> 24);
            uuid[i * 4 + 1] = (uint8_t)(bits >> 16);
            uuid[i * 4 + 2] = (uint8_t)(bits >> 8);
            uuid[i * 4 + 3] = (uint8_t)bits;
        }
        if (out->has_uuid && memcmp(out->uuid, uuid, sizeof(uuid)) != 0) return -1;
    }

    dimension = mc_nbt_compound_get(root, "Dimension");
    if (dimension && (dimension->type != MC_NBT_TAG_STRING ||
                      strcmp(dimension->payload.string_val, "minecraft:overworld") != 0)) return -1;
    abilities = mc_nbt_compound_get(root, "abilities");
    if (abilities) {
        if (abilities->type != MC_NBT_TAG_COMPOUND) return -1;
        const mc_nbt_tag_t *flying = mc_nbt_compound_get(abilities, "flying");
        if (flying && flying->type != MC_NBT_TAG_BYTE) return -1;
        out->flying = flying && flying->payload.byte_val != 0;
    }
    format = mc_nbt_compound_get(root, "mcInventoryFormat");
    if (format) {
        if (nbt_num_to_i32(format, &format_version) != 0 ||
            (format_version != 2 && format_version != 3)) return -1;
    } else if (mc_nbt_compound_get(root, "mcInventoryStateId")) {
        format_version = 1;
    }

    pos = mc_nbt_compound_get(root, "Pos");
    if (pos) {
        if (pos->type != MC_NBT_TAG_LIST || pos->payload.list.length != 3 ||
            nbt_num_to_f64(pos->payload.list.items[0], &px) != 0 ||
            nbt_num_to_f64(pos->payload.list.items[1], &py) != 0 ||
            nbt_num_to_f64(pos->payload.list.items[2], &pz) != 0 ||
            !isfinite(px) || !isfinite(py) || !isfinite(pz)) return -1;
    }

    rot = mc_nbt_compound_get(root, "Rotation");
    if (rot) {
        if (rot->type != MC_NBT_TAG_LIST || rot->payload.list.length != 2 ||
            nbt_num_to_f32(rot->payload.list.items[0], &yaw) != 0 ||
            nbt_num_to_f32(rot->payload.list.items[1], &pitch) != 0 ||
            !isfinite(yaw) || !isfinite(pitch)) return -1;
    }

    out->pos_x = px;
    out->pos_y = py;
    out->pos_z = pz;
    out->yaw = yaw;
    out->pitch = pitch;

    gamemode = mc_nbt_compound_get(root, "playerGameType");
    if (gamemode && (nbt_num_to_i32(gamemode, &out->gamemode) != 0 ||
                     out->gamemode < 0 || out->gamemode > 3)) return -1;

    health = mc_nbt_compound_get(root, "Health");
    if (health && (nbt_num_to_f32(health, &out->health) != 0 || !isfinite(out->health))) return -1;

    food_level = mc_nbt_compound_get(root, "foodLevel");
    if (food_level && (nbt_num_to_i32(food_level, &out->food_level) != 0 ||
                       out->food_level < 0 || out->food_level > 20)) return -1;

    food_saturation = mc_nbt_compound_get(root, "foodSaturationLevel");
    if (food_saturation && (nbt_num_to_f32(food_saturation, &out->food_saturation) != 0 ||
                            !isfinite(out->food_saturation))) return -1;

    food_exhaustion = mc_nbt_compound_get(root, "foodExhaustionLevel");
    if (food_exhaustion && (nbt_num_to_f32(food_exhaustion, &out->food_exhaustion) != 0 ||
                            !isfinite(out->food_exhaustion))) return -1;

    xp_progress = mc_nbt_compound_get(root, "XpP");
    if (xp_progress && (nbt_num_to_f32(xp_progress, &out->experience_progress) != 0 ||
                        !isfinite(out->experience_progress))) return -1;
    xp_level = mc_nbt_compound_get(root, "XpLevel");
    if (xp_level && nbt_num_to_i32(xp_level, &out->experience_level) != 0) return -1;
    xp_total = mc_nbt_compound_get(root, "XpTotal");
    if (xp_total && nbt_num_to_i32(xp_total, &out->experience_total) != 0) return -1;

    selected = mc_nbt_compound_get(root, "SelectedItemSlot");
    if (selected && (nbt_num_to_i32(selected, &out->inventory.selected_hotbar_slot) != 0 ||
                     out->inventory.selected_hotbar_slot < 0 ||
                     out->inventory.selected_hotbar_slot >= MC_PLAYER_HOTBAR_SIZE)) return -1;

    inventory_state = mc_nbt_compound_get(root, "mcInventoryStateId");
    if (inventory_state) (void)nbt_num_to_i32(inventory_state, &out->inventory.state_id);

    ender_state = mc_nbt_compound_get(root, "mcEnderStateId");
    if (ender_state) (void)nbt_num_to_i32(ender_state, &out->ender_state_id);

    inv = mc_nbt_compound_get(root, "Inventory");
    if (inv && parse_inventory_list(inv, out->inventory.slots, MC_PLAYER_SLOT_COUNT, true, format_version) != 0) return -1;

    equipment = mc_nbt_compound_get(root, "equipment");
    if (parse_equipment(equipment, &out->inventory) != 0) return -1;

    crafting = mc_nbt_compound_get(root, "mcCraftingItems");
    if (crafting && parse_inventory_list(crafting, &out->inventory.slots[1], 4, false, 3) != 0) return -1;

    cursor_item = mc_nbt_compound_get(root, "mcCursorItem");
    if (cursor_item && parse_single_slot_compound(cursor_item, &out->inventory.cursor_slot) != 0) return -1;

    ender = mc_nbt_compound_get(root, "EnderItems");
    if (ender && parse_inventory_list(ender, out->ender_chest, MC_CONTAINER_SLOT_COUNT, false, 3) != 0) return -1;

    return 0;
}

int mc_player_store_save_to_file(const char *world_path, const mc_player_data_t *player) {
    char dir_path[1024];
    char parent_path[1024];
    char file_path[1024];
    mc_nbt_tag_t *root = NULL;
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    int rc = -1;

    if (!world_path || !*world_path || !player) return -1;
    pthread_mutex_lock(&player_store_mutex);
    if (playerdata_dir_path(dir_path, sizeof(dir_path), world_path) != 0) goto cleanup;
    if (player_dat_path(file_path, sizeof(file_path), world_path, player, false) != 0) goto cleanup;
    int n = snprintf(parent_path, sizeof(parent_path), "%s/players", world_path);
    if (n < 0 || (size_t)n >= sizeof(parent_path)) goto cleanup;
    bool new_parent = access(parent_path, F_OK) != 0;
    bool new_directory = access(dir_path, F_OK) != 0;
    if (mkdir_p_local(dir_path, 0755) != 0) goto cleanup;
    if (new_directory && sync_directory(parent_path) != 0) goto cleanup;
    if (new_parent && sync_directory(world_path) != 0) goto cleanup;

    if (player_build_nbt(player, &root) != 0) goto cleanup;
    if (mc_nbt_write_named_root(root, &raw, &raw_len) != 0) goto cleanup;
    if (raw_len > MC_PLAYER_NBT_MAX_BYTES) goto cleanup;
    if (gzip_write_file_atomic(file_path, dir_path, raw, raw_len) != 0) goto cleanup;
    rc = 0;

cleanup:
    free(raw);
    mc_nbt_free(root);
    pthread_mutex_unlock(&player_store_mutex);
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
    size_t parsed_len = 0;
    mc_player_data_t tmp;
    int rc = 1;

    if (!world_path || !*world_path || !out) return 1;
    pthread_mutex_lock(&player_store_mutex);

    key.has_uuid = has_uuid;
    if (uuid) memcpy(key.uuid, uuid, sizeof(key.uuid));
    if (username) snprintf(key.username, sizeof(key.username), "%s", username);
    if (player_dat_path(file_path, sizeof(file_path), world_path, &key, false) != 0) { rc = -1; goto cleanup; }
    if (access(file_path, R_OK) != 0) {
        if (errno != ENOENT || player_dat_path(file_path, sizeof(file_path), world_path, &key, true) != 0) {
            rc = -1; goto cleanup;
        }
        if (access(file_path, R_OK) != 0) { rc = (errno == ENOENT) ? 1 : -1; goto cleanup; }
    }

    if (gzip_read_file(file_path, &raw, &raw_len) != 0) { rc = -1; goto cleanup; }
    if (mc_nbt_read_named_root(raw, raw_len, &root, &parsed_len) != 0 || parsed_len != raw_len) {
        rc = -1;
        goto cleanup;
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
        rc = -1;
        goto cleanup;
    }

    tmp.passthrough_nbt = root;
    root = NULL;
    mc_player_data_clear(out);
    *out = tmp;
    rc = 0;

cleanup:
    mc_nbt_free(root);
    free(raw);
    pthread_mutex_unlock(&player_store_mutex);
    return rc;
}
