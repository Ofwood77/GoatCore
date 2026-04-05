#include "generated_minecraft_ids.h"
#include "mc_inventory.h"
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

int main(void) {
    char world_template[] = "/tmp/mc_player_nbt_XXXXXX";
    char world_path[1024];
    char player_file[1200];
    char uuid_name[64];
    uint8_t gzip_magic[2] = {0};
    FILE *fp = NULL;
    mc_player_data_t original;
    mc_player_data_t loaded;
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

    original.has_uuid = true;
    memcpy(original.uuid, uuid, sizeof(uuid));
    snprintf(original.username, sizeof(original.username), "%s", "Roundtrip");
    original.gamemode = 1;
    original.pos_x = 123.45;
    original.pos_y = 64.0;
    original.pos_z = -12.3;
    original.yaw = 90.0f;
    original.pitch = 0.0f;
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

    if (mc_slot_set_simple(&original.inventory.slots[0], stone_id, 64) != 0 ||
        mc_slot_set_simple(&original.inventory.slots[10], sword_id, 1) != 0 ||
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
    snprintf(player_file, sizeof(player_file), "%s/playerdata/%s.dat", world_path, uuid_name);

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

    if (mc_player_store_load(world_path, uuid, true, original.username, &loaded) != 0) {
        printf("[FAIL] load from file\n");
        rc = 1;
        goto cleanup;
    }
    printf("[OK] load from file\n");

    if (check(loaded.gamemode == original.gamemode, "gamemode") != 0 ||
        check(same_f64(loaded.pos_x, original.pos_x), "position x") != 0 ||
        check(same_f64(loaded.pos_y, original.pos_y), "position y") != 0 ||
        check(same_f64(loaded.pos_z, original.pos_z), "position z") != 0 ||
        check(same_f32(loaded.yaw, original.yaw), "yaw") != 0 ||
        check(same_f32(loaded.pitch, original.pitch), "pitch") != 0 ||
        check(loaded.inventory.selected_hotbar_slot == original.inventory.selected_hotbar_slot, "selected slot") != 0 ||
        check(same_slot(&loaded.inventory.slots[0], &original.inventory.slots[0]), "inventory slot 0") != 0 ||
        check(same_slot(&loaded.inventory.slots[10], &original.inventory.slots[10]), "inventory slot 10") != 0 ||
        check(same_slot(&loaded.ender_chest[5], &original.ender_chest[5]), "ender chest slot 5") != 0) {
        rc = 1;
        goto cleanup;
    }

cleanup:
    if (fp) fclose(fp);
    mc_player_data_clear(&original);
    mc_player_data_clear(&loaded);
    return rc;
}
