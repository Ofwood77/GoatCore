#include "generated_minecraft_ids.h"
#include "mc_inventory.h"
#include "mc_nbt.h"
#include "mc_player_store.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

static bool fail_rename = false;
extern int __real_rename(const char *old_path, const char *new_path);
int __wrap_rename(const char *old_path, const char *new_path) {
    if (fail_rename) { errno = EIO; return -1; }
    return __real_rename(old_path, new_path);
}

static int copy_file(const char *from, const char *to) {
    FILE *src = fopen(from, "rb");
    FILE *dst = NULL;
    int rc = -1;
    if (!src) return -1;
    dst = fopen(to, "wb");
    if (!dst) goto done;
    for (;;) {
        uint8_t buf[4096];
        size_t n = fread(buf, 1, sizeof(buf), src);
        if (n && fwrite(buf, 1, n, dst) != n) goto done;
        if (n < sizeof(buf)) { if (ferror(src)) goto done; break; }
    }
    if (fflush(dst) != 0) goto done;
    rc = 0;
done:
    if (dst && fclose(dst) != 0) rc = -1;
    fclose(src);
    return rc;
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

static void uuid_to_path(char *buf, size_t cap, const uint8_t uuid[16]) {
    snprintf(buf, cap,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7], uuid[8], uuid[9], uuid[10],
             uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
}

static bool same_f64(double a, double b) {
    return fabs(a - b) < 1e-9;
}

static bool same_f32(float a, float b) {
    return fabsf(a - b) < 1e-6f;
}

static bool same_slot(const mc_slot_t *a, const mc_slot_t *b) {
    if (a->present != b->present) return false;
    if (!a->present) return true;
    return a->item_id == b->item_id && a->count == b->count;
}

static int check(bool cond, const char *label) {
    if (cond) {
        printf("[OK] %s\n", label);
        return 0;
    }
    printf("[FAIL] %s\n", label);
    return -1;
}

static int gzip_read_file_local(const char *path, uint8_t **out, size_t *out_len) {
    gzFile gz;
    size_t cap = 4096;
    size_t len = 0;
    uint8_t *buf = NULL;

    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!path || !out || !out_len) return -1;

    gz = gzopen(path, "rb");
    if (!gz) return -1;

    buf = (uint8_t *)malloc(cap);
    if (!buf) {
        gzclose(gz);
        return -1;
    }

    for (;;) {
        if (len == cap) {
            size_t next_cap = cap * 2;
            uint8_t *next = (uint8_t *)realloc(buf, next_cap);
            if (!next) {
                free(buf);
                gzclose(gz);
                return -1;
            }
            buf = next;
            cap = next_cap;
        }
        int n = gzread(gz, buf + len, (unsigned)(cap - len));
        if (n < 0) {
            free(buf);
            gzclose(gz);
            return -1;
        }
        if (n == 0) break;
        len += (size_t)n;
    }

    if (gzclose(gz) != Z_OK) {
        free(buf);
        return -1;
    }

    *out = buf;
    *out_len = len;
    return 0;
}

static int gzip_write_file_local(const char *path, const uint8_t *raw, size_t len) {
    gzFile gz = gzopen(path, "wb");
    if (!gz) return -1;
    if (len > UINT32_MAX || gzwrite(gz, raw, (unsigned)len) != (int)len) { gzclose(gz); return -1; }
    return gzclose(gz) == Z_OK ? 0 : -1;
}

static int add_test_damage_component(mc_nbt_tag_t *root) {
    mc_nbt_tag_t *inventory = (mc_nbt_tag_t *)mc_nbt_compound_get(root, "Inventory");
    if (!inventory || inventory->type != MC_NBT_TAG_LIST || inventory->payload.list.length < 1) return -1;
    mc_nbt_tag_t *item = inventory->payload.list.items[0];
    mc_nbt_tag_t *components = calloc(1, sizeof(*components));
    mc_nbt_tag_t *damage = calloc(1, sizeof(*damage));
    if (!components || !damage) { free(components); free(damage); return -1; }
    components->type = MC_NBT_TAG_COMPOUND;
    components->name = strdup("components");
    components->payload.compound.children = calloc(1, sizeof(mc_nbt_tag_t *));
    damage->type = MC_NBT_TAG_INT;
    damage->name = strdup("minecraft:damage");
    damage->payload.int_val = 5;
    if (!components->name || !components->payload.compound.children || !damage->name) {
        mc_nbt_free(components); mc_nbt_free(damage); return -1;
    }
    components->payload.compound.children[0] = damage;
    components->payload.compound.length = 1;
    int32_t n = item->payload.compound.length;
    mc_nbt_tag_t **children = realloc(item->payload.compound.children, (size_t)(n + 1) * sizeof(*children));
    if (!children) { mc_nbt_free(components); return -1; }
    item->payload.compound.children = children;
    item->payload.compound.children[n] = components;
    item->payload.compound.length++;
    return 0;
}

static int legacy_location_roundtrip(const char *world_path, const char *new_file,
                                     const uint8_t uuid[16], const char *username) {
    char old_dir[1200];
    char old_file[1300];
    char uuid_name[64];
    uint8_t *raw = NULL;
    uint8_t *modified = NULL;
    size_t raw_len = 0;
    size_t modified_len = 0;
    mc_nbt_tag_t *root = NULL;
    mc_player_data_t loaded;
    int rc = -1;

    mc_player_data_init(&loaded);
    if (gzip_read_file_local(new_file, &raw, &raw_len) != 0 ||
        mc_nbt_read_named_root(raw, raw_len, &root, NULL) != 0) goto done;
    mc_nbt_tag_t *format = (mc_nbt_tag_t *)mc_nbt_compound_get(root, "mcInventoryFormat");
    mc_nbt_tag_t *inventory = (mc_nbt_tag_t *)mc_nbt_compound_get(root, "Inventory");
    bool changed = false;
    if (!format || !inventory || inventory->type != MC_NBT_TAG_LIST) goto done;
    free(format->name);
    format->name = strdup("legacyFormatMarker");
    if (!format->name) goto done;
    for (int32_t i = 0; i < inventory->payload.list.length; i++) {
        mc_nbt_tag_t *slot = (mc_nbt_tag_t *)mc_nbt_compound_get(inventory->payload.list.items[i], "Slot");
        if (slot && slot->type == MC_NBT_TAG_BYTE && slot->payload.byte_val == 0) {
            slot->payload.byte_val = 36;
            changed = true;
        }
    }
    if (!changed || mc_nbt_write_named_root(root, &modified, &modified_len) != 0) goto done;
    uuid_to_path(uuid_name, sizeof(uuid_name), uuid);
    snprintf(old_dir, sizeof(old_dir), "%s/playerdata", world_path);
    snprintf(old_file, sizeof(old_file), "%s/%s.dat", old_dir, uuid_name);
    if (mkdir_p_local(old_dir, 0755) != 0 ||
        gzip_write_file_local(old_file, modified, modified_len) != 0 ||
        unlink(new_file) != 0) goto done;
    if (mc_player_store_load(world_path, uuid, true, username, &loaded) != 0) goto done;
    if (check(loaded.inventory.slots[36].present && loaded.inventory.slots[36].count == 64,
              "legacy GoatCore hotbar slot restored") != 0) goto done;
    if (mc_player_store_save(world_path, &loaded) != 0) goto done;
    if (check(access(old_file, F_OK) == 0 && access(new_file, F_OK) == 0,
              "legacy file retained while new path receives migrated save") != 0) goto done;
    rc = 0;
done:
    mc_player_data_clear(&loaded);
    mc_nbt_free(root);
    free(modified);
    free(raw);
    return rc;
}

static int audit_generated_nbt(const char *player_file) {
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    mc_nbt_tag_t *root = NULL;
    const mc_nbt_tag_t *pos;
    const mc_nbt_tag_t *rot;
    const mc_nbt_tag_t *inv;
    const mc_nbt_tag_t *gamemode;
    const mc_nbt_tag_t *first_item;
    const mc_nbt_tag_t *item_id;
    const mc_nbt_tag_t *item_count;
    const mc_nbt_tag_t *item_slot;
    const mc_nbt_tag_t *dimension;
    const mc_nbt_tag_t *format;
    const mc_nbt_tag_t *equipment;
    const mc_nbt_tag_t *version;
    const mc_nbt_tag_t *abilities;
    const mc_nbt_tag_t *identity;
    bool hotbar_found = false;
    int rc = -1;

    if (gzip_read_file_local(player_file, &raw, &raw_len) != 0) {
        printf("[FAIL] gzip read for nbt audit\n");
        goto cleanup;
    }
    if (mc_nbt_read_named_root(raw, raw_len, &root, NULL) != 0) {
        printf("[FAIL] nbt parse for audit\n");
        goto cleanup;
    }

    pos = mc_nbt_compound_get(root, "Pos");
    rot = mc_nbt_compound_get(root, "Rotation");
    inv = mc_nbt_compound_get(root, "Inventory");
    gamemode = mc_nbt_compound_get(root, "playerGameType");
    dimension = mc_nbt_compound_get(root, "Dimension");
    format = mc_nbt_compound_get(root, "mcInventoryFormat");
    equipment = mc_nbt_compound_get(root, "equipment");
    version = mc_nbt_compound_get(root, "DataVersion");
    abilities = mc_nbt_compound_get(root, "abilities");
    identity = mc_nbt_compound_get(root, "UUID");

    if (check(pos && pos->type == MC_NBT_TAG_LIST && pos->payload.list.elem_type == MC_NBT_TAG_DOUBLE &&
                  pos->payload.list.length == 3,
              "NBT tag Pos = List<Double>[3]") != 0)
        goto cleanup;
    if (check(rot && rot->type == MC_NBT_TAG_LIST && rot->payload.list.elem_type == MC_NBT_TAG_FLOAT &&
                  rot->payload.list.length == 2,
              "NBT tag Rotation = List<Float>[2]") != 0)
        goto cleanup;
    if (check(inv && inv->type == MC_NBT_TAG_LIST && inv->payload.list.elem_type == MC_NBT_TAG_COMPOUND, "NBT tag Inventory = List<Compound>") !=
        0)
        goto cleanup;
    if (check(gamemode && gamemode->type == MC_NBT_TAG_INT, "NBT tag playerGameType = Int") != 0) goto cleanup;
    if (check(dimension && dimension->type == MC_NBT_TAG_STRING &&
                  strcmp(dimension->payload.string_val, "minecraft:overworld") == 0, "overworld dimension") != 0) goto cleanup;
    if (check(format && format->type == MC_NBT_TAG_INT && format->payload.int_val == 3,
              "Vanilla inventory slot format") != 0) goto cleanup;
    if (check(version && version->type == MC_NBT_TAG_INT && version->payload.int_val == 4788,
              "26.1.1 player DataVersion") != 0) goto cleanup;
    if (check(identity && identity->type == MC_NBT_TAG_INT_ARRAY && identity->payload.int_array.length == 4,
              "Vanilla UUID int array") != 0) goto cleanup;
    const mc_nbt_tag_t *flying = abilities ? mc_nbt_compound_get(abilities, "flying") : NULL;
    if (check(flying && flying->type == MC_NBT_TAG_BYTE && flying->payload.byte_val == 1,
              "creative flight in abilities") != 0) goto cleanup;

    first_item = (inv && inv->payload.list.length > 0) ? inv->payload.list.items[0] : NULL;
    item_id = first_item ? mc_nbt_compound_get(first_item, "id") : NULL;
    item_count = first_item ? mc_nbt_compound_get(first_item, "count") : NULL;
    item_slot = first_item ? mc_nbt_compound_get(first_item, "Slot") : NULL;

    if (check(first_item && first_item->type == MC_NBT_TAG_COMPOUND, "Inventory entry = Compound") != 0) goto cleanup;
    if (check(item_id && item_id->type == MC_NBT_TAG_STRING, "Inventory.id = String") != 0) goto cleanup;
    if (check(item_count && item_count->type == MC_NBT_TAG_INT, "Inventory.count = Int") != 0) goto cleanup;
    if (check(item_slot && item_slot->type == MC_NBT_TAG_BYTE, "Inventory.Slot = Byte") != 0) goto cleanup;
    for (int32_t i = 0; i < inv->payload.list.length; i++) {
        const mc_nbt_tag_t *entry = inv->payload.list.items[i];
        const mc_nbt_tag_t *slot = mc_nbt_compound_get(entry, "Slot");
        if (!slot || slot->type != MC_NBT_TAG_BYTE) continue;
        hotbar_found |= slot->payload.byte_val == 0;
    }
    if (check(hotbar_found && equipment && mc_nbt_compound_get(equipment, "head") &&
                  mc_nbt_compound_get(equipment, "offhand"), "hotbar and separate Vanilla equipment") != 0) goto cleanup;

    rc = 0;

cleanup:
    mc_nbt_free(root);
    free(raw);
    return rc;
}

static int reference_fixture_roundtrip(const char *world_path) {
    static const uint8_t uuid[16] = {
        0x3c, 0x6a, 0xa7, 0xc1, 0x78, 0x54, 0x35, 0xee,
        0x93, 0xa8, 0xeb, 0x4d, 0x69, 0x35, 0x5d, 0x2d,
    };
    char dir[1200];
    char file[1300];
    char corrupt_file[1300];
    char id[64];
    mc_player_data_t player;
    mc_player_data_t restored;
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    uint8_t *unsupported = NULL;
    size_t unsupported_len = 0;
    uint8_t *after = NULL;
    size_t after_len = 0;
    mc_nbt_tag_t *root = NULL;
    int rc = -1;

    mc_player_data_init(&player);
    mc_player_data_init(&restored);
    uuid_to_path(id, sizeof(id), uuid);
    snprintf(dir, sizeof(dir), "%s/players/data", world_path);
    snprintf(file, sizeof(file), "%s/%s.dat", dir, id);
    if (mkdir_p_local(dir, 0755) != 0 ||
        copy_file("tests/fixtures/vanilla-26.1.1-player.dat", file) != 0) goto done;
    if (mc_player_store_load(world_path, uuid, true, "StressVE00000", &player) != 0) goto done;
    if (check(player.inventory.slots[36].item_id == mc_minecraft_item_id("minecraft:stone") &&
                  player.inventory.slots[36].count == 64 &&
                  player.inventory.slots[5].item_id == mc_minecraft_item_id("minecraft:diamond_helmet") &&
                  player.inventory.slots[45].item_id == mc_minecraft_item_id("minecraft:diamond_sword"),
              "load official 26.1.1 inventory and equipment") != 0) goto done;
    if (mc_player_store_save(world_path, &player) != 0 ||
        mc_player_store_load(world_path, uuid, true, "StressVE00000", &restored) != 0) goto done;
    if (check(same_slot(&player.inventory.slots[36], &restored.inventory.slots[36]) &&
                  same_slot(&player.inventory.slots[5], &restored.inventory.slots[5]) &&
                  same_slot(&player.inventory.slots[45], &restored.inventory.slots[45]),
              "official inventory survives GoatCore save and reload") != 0) goto done;
    if (gzip_read_file_local(file, &raw, &raw_len) != 0 ||
        mc_nbt_read_named_root(raw, raw_len, &root, NULL) != 0) goto done;
    if (check(mc_nbt_compound_get(root, "recipeBook") != NULL &&
                  mc_nbt_compound_get(root, "attributes") != NULL,
              "unhandled official NBT fields survive") != 0) goto done;

    fail_rename = true;
    player.pos_x = 888.0;
    if (check(mc_player_store_save(world_path, &player) != 0, "injected rename failure") != 0) goto done;
    fail_rename = false;
    if (mc_player_store_load(world_path, uuid, true, "StressVE00000", &restored) != 0) goto done;
    if (check(restored.pos_x != 888.0, "failed save keeps prior player file") != 0) goto done;
    float saved_health = player.health;
    player.health = NAN;
    if (check(mc_player_store_save(world_path, &player) == -1,
              "invalid in-memory state cannot overwrite valid save") != 0) goto done;
    player.health = saved_health;

    snprintf(corrupt_file, sizeof(corrupt_file), "%s/00000000-0000-0000-0000-000000000001.dat", dir);
    if (copy_file(file, corrupt_file) != 0) goto done;
    uint8_t corrupt_uuid[16] = {0};
    corrupt_uuid[15] = 1;
    if (check(mc_player_store_load(world_path, corrupt_uuid, true, "WrongIdentity", &restored) == -1,
              "file UUID must match requested identity") != 0) goto done;
    struct stat st;
    if (stat(corrupt_file, &st) != 0 || truncate(corrupt_file, st.st_size / 2) != 0) goto done;
    if (check(mc_player_store_load(world_path, corrupt_uuid, true, "Corrupt", &restored) == -1,
              "truncated player NBT rejected") != 0) goto done;
    if (check(stat(corrupt_file, &st) == 0 && st.st_size > 0,
              "corrupt file retained for recovery") != 0) goto done;
    if (add_test_damage_component(root) != 0 ||
        mc_nbt_write_named_root(root, &unsupported, &unsupported_len) != 0 ||
        gzip_write_file_local(file, unsupported, unsupported_len) != 0) goto done;
    if (check(mc_player_store_load(world_path, uuid, true, "Unsupported", &restored) == -1,
              "unsupported item components rejected") != 0) goto done;
    if (gzip_read_file_local(file, &after, &after_len) != 0) goto done;
    if (check(after_len == unsupported_len && memcmp(after, unsupported, after_len) == 0,
              "unsupported item file preserved byte for byte") != 0) goto done;
    rc = 0;
done:
    fail_rename = false;
    mc_nbt_free(root);
    free(raw);
    free(unsupported);
    free(after);
    mc_player_data_clear(&player);
    mc_player_data_clear(&restored);
    return rc;
}

int main(void) {
    char world_template[] = "/tmp/mc_player_nbt_XXXXXX";
    char world_path[1024];
    char player_file[1200];
    char uuid_name[64];
    uint8_t gzip_magic[2] = {0};
    FILE *fp = NULL;
    mc_player_data_t original;
    mc_player_data_t loaded;
    mc_player_data_t same_name_other_uuid;
    mc_player_data_t other_loaded;
    uint8_t uuid[16] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0,
                        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    int stone_id;
    int sword_id;
    int rc = 0;

    if (!mkdtemp(world_template)) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(world_path, sizeof(world_path), "%s", world_template);
    if (mkdir_p_local(world_path, 0755) != 0) {
        perror("mkdir_p_local");
        return 1;
    }

    mc_player_data_init(&original);
    mc_player_data_init(&loaded);
    mc_player_data_init(&same_name_other_uuid);
    mc_player_data_init(&other_loaded);

    original.has_uuid = true;
    memcpy(original.uuid, uuid, sizeof(uuid));
    snprintf(original.username, sizeof(original.username), "%s", "Roundtrip");
    original.gamemode = 1;
    original.flying = true;
    original.health = 17.5f;
    original.food_level = 13;
    original.food_saturation = 3.25f;
    original.food_exhaustion = 2.75f;
    original.pos_x = 123.45;
    original.pos_y = 64.0;
    original.pos_z = -12.3;
    original.yaw = 90.0f;
    original.pitch = 0.0f;
    original.experience_progress = 0.4f;
    original.experience_level = 7;
    original.experience_total = 120;
    original.inventory.selected_hotbar_slot = 2;
    original.inventory.state_id = 7;
    original.ender_state_id = 3;

    stone_id = mc_minecraft_item_id("minecraft:stone");
    sword_id = mc_minecraft_item_id("minecraft:diamond_sword");
    if (stone_id <= 0 || sword_id <= 0) {
        printf("[FAIL] item id lookup\n");
        rc = 1;
        goto cleanup;
    }

    if (mc_slot_set_simple(&original.inventory.slots[36], stone_id, 64) != 0 ||
        mc_slot_set_simple(&original.inventory.slots[1], stone_id, 3) != 0 ||
        mc_slot_set_simple(&original.inventory.slots[0], stone_id, 12) != 0 ||
        mc_slot_set_simple(&original.inventory.slots[10], sword_id, 1) != 0 ||
        mc_slot_set_simple(&original.inventory.slots[5], sword_id, 1) != 0 ||
        mc_slot_set_simple(&original.inventory.slots[45], stone_id, 32) != 0 ||
        mc_slot_set_simple(&original.ender_chest[5], stone_id, 32) != 0) {
        printf("[FAIL] slot init\n");
        rc = 1;
        goto cleanup;
    }

    if (mc_player_store_save_to_file(world_path, &original) != 0) {
        printf("[FAIL] save to file\n");
        rc = 1;
        goto cleanup;
    }
    printf("[OK] save to file\n");

    uuid_to_path(uuid_name, sizeof(uuid_name), uuid);
    snprintf(player_file, sizeof(player_file), "%s/players/data/%s.dat", world_path, uuid_name);

    fp = fopen(player_file, "rb");
    if (!fp) {
        perror("fopen");
        rc = 1;
        goto cleanup;
    }
    if (fread(gzip_magic, 1, sizeof(gzip_magic), fp) != sizeof(gzip_magic)) {
        printf("[FAIL] read gzip magic\n");
        rc = 1;
        goto cleanup;
    }
    fclose(fp);
    fp = NULL;
    if (check(gzip_magic[0] == 0x1f && gzip_magic[1] == 0x8b, "gzip magic 1F 8B") != 0) {
        rc = 1;
        goto cleanup;
    }

    if (audit_generated_nbt(player_file) != 0) {
        rc = 1;
        goto cleanup;
    }

    if (mc_player_store_load(world_path, uuid, true, original.username, &loaded) != 0) {
        printf("[FAIL] load from file\n");
        rc = 1;
        goto cleanup;
    }
    printf("[OK] load from file\n");

    if (check(loaded.gamemode == original.gamemode, "gamemode") != 0 ||
        check(loaded.flying == original.flying, "flying") != 0 ||
        check(same_f32(loaded.health, original.health), "health") != 0 ||
        check(loaded.food_level == original.food_level, "food level") != 0 ||
        check(same_f32(loaded.food_saturation, original.food_saturation), "saturation") != 0 ||
        check(same_f32(loaded.food_exhaustion, original.food_exhaustion), "exhaustion") != 0 ||
        check(same_f64(loaded.pos_x, original.pos_x), "position x") != 0 ||
        check(same_f64(loaded.pos_y, original.pos_y), "position y") != 0 ||
        check(same_f64(loaded.pos_z, original.pos_z), "position z") != 0 ||
        check(same_f32(loaded.yaw, original.yaw), "yaw") != 0 ||
        check(same_f32(loaded.pitch, original.pitch), "pitch") != 0 ||
        check(same_f32(loaded.experience_progress, original.experience_progress), "experience progress") != 0 ||
        check(loaded.experience_level == original.experience_level, "experience level") != 0 ||
        check(loaded.experience_total == original.experience_total, "experience total") != 0 ||
        check(loaded.inventory.selected_hotbar_slot == original.inventory.selected_hotbar_slot, "selected slot") != 0 ||
        check(same_slot(&loaded.inventory.slots[36], &original.inventory.slots[36]), "hotbar slot 0") != 0 ||
        check(same_slot(&loaded.inventory.slots[1], &original.inventory.slots[1]) &&
                  !loaded.inventory.slots[0].present, "crafting inputs survive without duplicating result") != 0 ||
        check(same_slot(&loaded.inventory.slots[10], &original.inventory.slots[10]), "inventory slot 10") != 0 ||
        check(same_slot(&loaded.inventory.slots[5], &original.inventory.slots[5]), "helmet slot") != 0 ||
        check(same_slot(&loaded.inventory.slots[45], &original.inventory.slots[45]), "offhand slot") != 0 ||
        check(same_slot(&loaded.ender_chest[5], &original.ender_chest[5]), "ender chest slot 5") != 0) {
        rc = 1;
        goto cleanup;
    }
    if (legacy_location_roundtrip(world_path, player_file, uuid, original.username) != 0) {
        rc = 1;
        goto cleanup;
    }
    if (reference_fixture_roundtrip(world_path) != 0) {
        rc = 1;
        goto cleanup;
    }
    same_name_other_uuid.has_uuid = true;
    memcpy(same_name_other_uuid.uuid, uuid, sizeof(uuid));
    same_name_other_uuid.uuid[15] ^= 0x55;
    snprintf(same_name_other_uuid.username, sizeof(same_name_other_uuid.username), "%s", original.username);
    same_name_other_uuid.pos_x = -99.25;
    if (mc_slot_set_simple(&same_name_other_uuid.inventory.slots[36], stone_id, 13) != 0 ||
        mc_player_store_save(world_path, &same_name_other_uuid) != 0 ||
        mc_player_store_load(world_path, same_name_other_uuid.uuid, true, original.username, &other_loaded) != 0 ||
        mc_player_store_load(world_path, uuid, true, original.username, &loaded) != 0) {
        rc = 1;
        goto cleanup;
    }
    if (check(same_f64(other_loaded.pos_x, -99.25) &&
                  other_loaded.inventory.slots[36].count == 13 &&
                  same_f64(loaded.pos_x, original.pos_x) && loaded.inventory.slots[36].count == 64,
              "same name with different UUIDs keeps separate saves") != 0) {
        rc = 1;
        goto cleanup;
    }

cleanup:
    if (fp) fclose(fp);
    mc_player_data_clear(&original);
    mc_player_data_clear(&loaded);
    mc_player_data_clear(&same_name_other_uuid);
    mc_player_data_clear(&other_loaded);
    return rc;
}
