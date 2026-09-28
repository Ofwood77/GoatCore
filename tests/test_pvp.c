#include "mc_net.h"
#include "mc_protocol.h"
#include "generated_minecraft_ids.h"

#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GAMEMODE_SURVIVAL 0
#define GAMEMODE_CREATIVE 1
#define GAMEMODE_SPECTATOR 3

static int g_drop_calls = 0;
static int g_drop_items = 0;

int conn_write_packet(mc_conn_t *c, int32_t packet_id, const uint8_t *payload, size_t payload_len, int compression_threshold) {
    (void)c;
    (void)packet_id;
    (void)payload;
    (void)payload_len;
    (void)compression_threshold;
    return 0;
}

void conn_close(mc_conn_t *c) {
    (void)c;
}

mc_conn_t *net_server_find_conn_by_name(mc_server_t *server, const char *name) {
    (void)server;
    (void)name;
    return NULL;
}

mc_conn_t *net_server_find_conn_by_entity_id(mc_server_t *server, int32_t entity_id) {
    (void)server;
    (void)entity_id;
    return NULL;
}

void net_server_release_conn(mc_conn_t *conn) {
    (void)conn;
}

mc_world_t *net_server_world(mc_server_t *server) {
    (void)server;
    return NULL;
}

int net_server_spawn_item_drop(mc_server_t *server, double x, double y, double z, const mc_slot_t *slot) {
    (void)server;
    (void)x;
    (void)y;
    (void)z;
    if (slot && slot->present && slot->count > 0) {
        g_drop_calls++;
        g_drop_items += slot->count;
    }
    return 0;
}

int net_server_spawn_item_drop_locked(mc_server_t *server, double x, double y, double z, const mc_slot_t *slot) {
    return net_server_spawn_item_drop(server, x, y, z, slot);
}

int net_server_spawn_item_drop_with_motion(mc_server_t *server, double x, double y, double z, double vx, double vy, double vz,
                                           const mc_slot_t *slot, int32_t pickup_delay_ticks) {
    (void)vx;
    (void)vy;
    (void)vz;
    (void)pickup_delay_ticks;
    return net_server_spawn_item_drop(server, x, y, z, slot);
}

int net_server_sync_item_entities_to_conn(mc_server_t *server, mc_conn_t *conn) {
    (void)server;
    (void)conn;
    return 0;
}

int net_server_resolve_item_entities_for_block(mc_server_t *server, int32_t x, int32_t y, int32_t z, int32_t state_id) {
    (void)server;
    (void)x;
    (void)y;
    (void)z;
    (void)state_id;
    return 0;
}

mc_difficulty_t net_server_get_difficulty(mc_server_t *server) {
    (void)server;
    return MC_DIFFICULTY_NORMAL;
}

void net_server_set_difficulty(mc_server_t *server, mc_difficulty_t difficulty) {
    (void)server;
    (void)difficulty;
}

const char *mc_difficulty_name(mc_difficulty_t difficulty) {
    switch (difficulty) {
        case MC_DIFFICULTY_PEACEFUL: return "peaceful";
        case MC_DIFFICULTY_EASY: return "easy";
        case MC_DIFFICULTY_NORMAL: return "normal";
        case MC_DIFFICULTY_HARD: return "hard";
    }
    return "normal";
}

int net_server_broadcast_difficulty(mc_server_t *server) {
    (void)server;
    return 0;
}

int net_server_broadcast_system_message(mc_server_t *server, const char *text) {
    (void)server;
    (void)text;
    return 0;
}

void net_server_close_container_viewers(mc_server_t *server, mc_container_kind_t kind, int32_t x, int32_t y, int32_t z) {
    (void)server;
    (void)kind;
    (void)x;
    (void)y;
    (void)z;
}

int net_server_get_open_container_snapshot(mc_server_t *server, mc_container_kind_t kind, int32_t x, int32_t y, int32_t z,
                                           mc_container_instance_t *out) {
    (void)server;
    (void)kind;
    (void)x;
    (void)y;
    (void)z;
    (void)out;
    return 1;
}

static void test_conn_init(mc_conn_t *c, mc_player_data_t *player, int32_t entity_id) {
    memset(c, 0, sizeof(*c));
    mc_player_data_init(player);
    c->fd = -1;
    c->entity_id = entity_id;
    c->player = player;
    c->gamemode = GAMEMODE_SURVIVAL;
    c->health = 20.0f;
    c->food = 20;
    c->food_saturation = 5.0f;
    c->x = 0.0;
    c->y = 64.0;
    c->z = 0.0;
    c->has_pos = true;
    c->play_ready = true;
    atomic_init(&c->state, MC_STATE_PLAY);
    atomic_init(&c->closing, false);
    atomic_init(&c->refcount, 1);
    assert(pthread_mutex_init(&c->out_lock, NULL) == 0);
    player->health = c->health;
    player->gamemode = c->gamemode;
}

static void test_conn_clear(mc_conn_t *c, mc_player_data_t *player) {
    pthread_mutex_destroy(&c->out_lock);
    mc_player_data_clear(player);
}

static void put_item_count(mc_conn_t *c, int slot, const char *name, int32_t count) {
    int32_t item_id = mc_minecraft_item_id(name);
    assert(item_id >= 0);
    assert(slot >= 0 && slot < MC_PLAYER_SLOT_COUNT);
    mc_slot_clear(&c->player->inventory.slots[slot]);
    assert(mc_slot_set_simple(&c->player->inventory.slots[slot], item_id, count) == 0);
}

static void put_item(mc_conn_t *c, int slot, const char *name) {
    put_item_count(c, slot, name, 1);
}

static size_t write_varint_local(uint8_t *buf, size_t cap, int32_t v) {
    size_t n = 0;
    assert(varint_write(buf, cap, v, &n) == 0);
    return n;
}

static void test_serverbound_play_packet_ids(void) {
    assert(proto_play_serverbound_packet_id_for_test("chat_command") == 0x07);
    assert(proto_play_serverbound_packet_id_for_test("chat_message") == 0x09);
    assert(proto_play_serverbound_packet_id_for_test("interact") == 0x1a);
    assert(proto_play_serverbound_packet_id_for_test("move_pos") == 0x1e);
    assert(proto_play_serverbound_packet_id_for_test("move_pos_rot") == 0x1f);
    assert(proto_play_serverbound_packet_id_for_test("move_rot") == 0x20);
    assert(proto_play_serverbound_packet_id_for_test("move_on_ground") == 0x21);
    assert(proto_play_serverbound_packet_id_for_test("player_action") == 0x29);
    assert(proto_play_serverbound_packet_id_for_test("use_item_on") == 0x42);
    assert(proto_play_serverbound_packet_id_for_test("use_item") == 0x43);
}

static void test_interact_parse(void) {
    uint8_t buf[16];
    size_t pos = 0;
    int32_t target = -1;
    bool is_attack = false;
    pos += write_varint_local(buf + pos, sizeof(buf) - pos, 123);
    pos += write_varint_local(buf + pos, sizeof(buf) - pos, 1);
    buf[pos++] = 0;
    assert(proto_play_parse_interact_attack_for_test(buf, pos, &target, &is_attack) == 0);
    assert(target == 123);
    assert(is_attack);
}

static void test_pvp_damage_and_gates(void) {
    mc_conn_t attacker, target;
    mc_player_data_t attacker_player, target_player;
    test_conn_init(&attacker, &attacker_player, 1);
    test_conn_init(&target, &target_player, 2);
    attacker.x = 0.0;
    attacker.y = 64.0;
    attacker.z = 0.0;
    target.x = 0.0;
    target.y = 64.0;
    target.z = 2.0;

    assert(proto_play_apply_pvp_attack_for_test(&attacker, &target, true, 0) == 1);
    assert(fabsf(target.health - 16.0f) < 0.01f);

    target.health = target.player->health = 20.0f;
    assert(proto_play_apply_pvp_attack_for_test(&attacker, &target, false, 0) == 0);
    assert(fabsf(target.health - 20.0f) < 0.01f);

    attacker.x = 20.0;
    assert(proto_play_apply_pvp_attack_for_test(&attacker, &target, true, 0) == 0);
    assert(fabsf(target.health - 20.0f) < 0.01f);
    attacker.x = 0.0;

    attacker.gamemode = GAMEMODE_CREATIVE;
    assert(proto_play_apply_pvp_attack_for_test(&attacker, &target, true, 0) == 0);
    assert(fabsf(target.health - 20.0f) < 0.01f);
    attacker.gamemode = GAMEMODE_SURVIVAL;

    target.gamemode = GAMEMODE_SPECTATOR;
    assert(proto_play_apply_pvp_attack_for_test(&attacker, &target, true, 0) == 0);
    assert(fabsf(target.health - 20.0f) < 0.01f);

    test_conn_clear(&attacker, &attacker_player);
    test_conn_clear(&target, &target_player);
}

static void test_armor_reduces_damage(void) {
    mc_conn_t attacker, target;
    mc_player_data_t attacker_player, target_player;
    test_conn_init(&attacker, &attacker_player, 1);
    test_conn_init(&target, &target_player, 2);

    float none = proto_play_pvp_damage_for_test(&attacker, &target, 10.0f);
    put_item(&target, 5, "minecraft:leather_helmet");
    put_item(&target, 6, "minecraft:leather_chestplate");
    put_item(&target, 7, "minecraft:leather_leggings");
    put_item(&target, 8, "minecraft:leather_boots");
    float leather = proto_play_pvp_damage_for_test(&attacker, &target, 10.0f);
    put_item(&target, 5, "minecraft:netherite_helmet");
    put_item(&target, 6, "minecraft:netherite_chestplate");
    put_item(&target, 7, "minecraft:netherite_leggings");
    put_item(&target, 8, "minecraft:netherite_boots");
    float netherite = proto_play_pvp_damage_for_test(&attacker, &target, 10.0f);

    assert(fabsf(none - 10.0f) < 0.01f);
    assert(leather < none);
    assert(netherite < leather);
    assert(fabsf(netherite - 2.0f) < 0.01f);

    test_conn_clear(&attacker, &attacker_player);
    test_conn_clear(&target, &target_player);
}

static void test_shield_blocking(void) {
    mc_conn_t attacker, target;
    mc_player_data_t attacker_player, target_player;
    test_conn_init(&attacker, &attacker_player, 1);
    test_conn_init(&target, &target_player, 2);
    put_item(&target, 45, "minecraft:shield");

    target.is_using_item = false;
    target.using_hand = 1;
    target.using_slot = 45;
    target.yaw = 0.0f;
    target.x = 0.0;
    target.z = 0.0;
    attacker.x = 0.0;
    attacker.z = 2.0;
    assert(proto_play_pvp_damage_for_test(&attacker, &target, 6.0f) > 0.0f);

    target.is_using_item = true;
    assert(fabsf(proto_play_pvp_damage_for_test(&attacker, &target, 6.0f)) < 0.01f);

    attacker.z = -2.0;
    assert(proto_play_pvp_damage_for_test(&attacker, &target, 6.0f) > 0.0f);

    test_conn_clear(&attacker, &attacker_player);
    test_conn_clear(&target, &target_player);
}

static void reset_drop_counters(void) {
    g_drop_calls = 0;
    g_drop_items = 0;
}

static void test_lifecycle_death_empty_inventory(void) {
    mc_conn_t player_conn;
    mc_player_data_t player;
    mc_server_config_t cfg = {0};
    test_conn_init(&player_conn, &player, 10);
    cfg.keep_inventory = false;
    player_conn.cfg = &cfg;
    player_conn.server = (mc_server_t *)0x1;

    reset_drop_counters();
    assert(proto_play_inventory_stack_count_for_test(&player_conn) == 0);
    assert(proto_play_apply_damage_for_test(&player_conn, 25.0f, false, 0) == 0);
    assert(player_conn.dead);
    assert(fabsf(player_conn.health) < 0.01f);
    assert(g_drop_calls == 0);
    assert(g_drop_items == 0);
    assert(proto_play_inventory_stack_count_for_test(&player_conn) == 0);
    assert(!proto_play_can_pickup_items_for_test(&player_conn));

    test_conn_clear(&player_conn, &player);
}

static void test_lifecycle_death_drops_once_and_clears_inventory(void) {
    mc_conn_t player_conn;
    mc_player_data_t player;
    mc_server_config_t cfg = {0};
    test_conn_init(&player_conn, &player, 11);
    cfg.keep_inventory = false;
    player_conn.cfg = &cfg;
    player_conn.server = (mc_server_t *)0x1;

    put_item_count(&player_conn, 36, "minecraft:stone", 3);
    put_item(&player_conn, 5, "minecraft:diamond_helmet");
    put_item(&player_conn, 45, "minecraft:shield");
    assert(mc_slot_set_simple(&player_conn.player->inventory.cursor_slot, mc_minecraft_item_id("minecraft:dirt"), 2) == 0);
    assert(proto_play_inventory_stack_count_for_test(&player_conn) == 4);

    reset_drop_counters();
    assert(proto_play_apply_damage_for_test(&player_conn, 25.0f, false, 0) == 0);
    assert(player_conn.dead);
    assert(g_drop_calls == 4);
    assert(g_drop_items == 7);
    assert(proto_play_inventory_stack_count_for_test(&player_conn) == 0);

    assert(proto_play_apply_damage_for_test(&player_conn, 25.0f, false, 0) == 0);
    assert(g_drop_calls == 4);
    assert(g_drop_items == 7);

    test_conn_clear(&player_conn, &player);
}

static void test_lifecycle_keep_inventory(void) {
    mc_conn_t player_conn;
    mc_player_data_t player;
    mc_server_config_t cfg = {0};
    test_conn_init(&player_conn, &player, 12);
    cfg.keep_inventory = true;
    player_conn.cfg = &cfg;
    player_conn.server = (mc_server_t *)0x1;

    put_item_count(&player_conn, 36, "minecraft:stone", 3);
    put_item(&player_conn, 45, "minecraft:shield");
    reset_drop_counters();
    assert(proto_play_apply_damage_for_test(&player_conn, 25.0f, false, 0) == 0);
    assert(player_conn.dead);
    assert(g_drop_calls == 0);
    assert(g_drop_items == 0);
    assert(proto_play_inventory_stack_count_for_test(&player_conn) == 2);

    test_conn_clear(&player_conn, &player);
}

static void test_lifecycle_respawn_reset(void) {
    mc_conn_t player_conn;
    mc_player_data_t player;
    test_conn_init(&player_conn, &player, 13);
    player_conn.gamemode = GAMEMODE_CREATIVE;
    player.gamemode = GAMEMODE_CREATIVE;
    player_conn.dead = true;
    player_conn.health = 0.0f;
    player_conn.food = 3;
    player_conn.food_saturation = 0.0f;
    player_conn.food_exhaustion = 9.0f;
    player_conn.is_using_item = true;
    player_conn.using_hand = 1;
    player_conn.next_void_damage_ms = 123;
    player_conn.next_natural_regen_ms = 456;
    player_conn.next_starvation_damage_ms = 789;
    player_conn.fall_tracking = true;

    proto_play_respawn_state_reset_for_test(&player_conn);
    assert(!player_conn.dead);
    assert(fabsf(player_conn.health - 20.0f) < 0.01f);
    assert(player_conn.food == 20);
    assert(fabsf(player_conn.food_saturation - 5.0f) < 0.01f);
    assert(fabsf(player_conn.food_exhaustion) < 0.01f);
    assert(player_conn.gamemode == GAMEMODE_CREATIVE);
    assert(!player_conn.is_using_item);
    assert(player_conn.next_void_damage_ms == 0);
    assert(player_conn.next_natural_regen_ms == 0);
    assert(player_conn.next_starvation_damage_ms == 0);
    assert(!player_conn.fall_tracking);
    assert(proto_play_can_pickup_items_for_test(&player_conn));

    test_conn_clear(&player_conn, &player);
}

static void test_pickup_inventory_empty_merge_full_and_spectator(void) {
    mc_conn_t player_conn;
    mc_player_data_t player;
    test_conn_init(&player_conn, &player, 20);

    int32_t stone_id = mc_minecraft_item_id("minecraft:stone");
    int32_t dirt_id = mc_minecraft_item_id("minecraft:dirt");
    assert(stone_id >= 0 && dirt_id >= 0);

    mc_slot_t ground = {0};
    assert(mc_slot_set_simple(&ground, stone_id, 5) == 0);
    assert(proto_play_try_pickup_ground_slot(&player_conn, &ground) == 5);
    assert(!ground.present);
    assert(player.inventory.slots[36].present);
    assert(player.inventory.slots[36].item_id == stone_id);
    assert(player.inventory.slots[36].count == 5);

    assert(mc_slot_set_simple(&ground, stone_id, 60) == 0);
    assert(proto_play_try_pickup_ground_slot(&player_conn, &ground) == 60);
    assert(!ground.present);
    assert(player.inventory.slots[36].count == 64);
    assert(player.inventory.slots[37].present);
    assert(player.inventory.slots[37].item_id == stone_id);
    assert(player.inventory.slots[37].count == 1);

    for (int i = 0; i < MC_PLAYER_SLOT_COUNT; i++) {
        assert(mc_slot_set_simple(&player.inventory.slots[i], dirt_id, 64) == 0);
    }
    assert(mc_slot_set_simple(&ground, stone_id, 3) == 0);
    assert(proto_play_try_pickup_ground_slot(&player_conn, &ground) == 0);
    assert(ground.present && ground.count == 3);
    mc_slot_clear(&ground);

    for (int i = 0; i < MC_PLAYER_SLOT_COUNT; i++) mc_slot_clear(&player.inventory.slots[i]);
    player_conn.gamemode = GAMEMODE_SPECTATOR;
    assert(mc_slot_set_simple(&ground, stone_id, 2) == 0);
    assert(proto_play_try_pickup_ground_slot(&player_conn, &ground) == 0);
    assert(ground.present && ground.count == 2);
    assert(!proto_play_can_pickup_items_for_test(&player_conn));
    mc_slot_clear(&ground);

    test_conn_clear(&player_conn, &player);
}

static void test_manual_drop_selected_item_stack_empty_and_dead(void) {
    mc_conn_t player_conn;
    mc_player_data_t player;
    test_conn_init(&player_conn, &player, 21);
    player_conn.server = (mc_server_t *)0x1;
    player.inventory.selected_hotbar_slot = 0;

    reset_drop_counters();
    put_item_count(&player_conn, 36, "minecraft:stone", 3);
    assert(proto_play_drop_selected_for_test(&player_conn, 1) == 1);
    assert(player.inventory.slots[36].present);
    assert(player.inventory.slots[36].count == 2);
    assert(g_drop_calls == 1);
    assert(g_drop_items == 1);

    assert(proto_play_drop_selected_for_test(&player_conn, 64) == 1);
    assert(!player.inventory.slots[36].present);
    assert(g_drop_calls == 2);
    assert(g_drop_items == 3);

    assert(proto_play_drop_selected_for_test(&player_conn, 1) == 0);
    assert(g_drop_calls == 2);
    assert(g_drop_items == 3);

    put_item_count(&player_conn, 36, "minecraft:stone", 2);
    player_conn.dead = true;
    assert(proto_play_drop_selected_for_test(&player_conn, 1) == 0);
    assert(player.inventory.slots[36].present && player.inventory.slots[36].count == 2);
    assert(g_drop_calls == 2);
    assert(g_drop_items == 3);

    test_conn_clear(&player_conn, &player);
}

static void test_creative_set_slot_gate(void) {
    mc_conn_t player_conn;
    mc_player_data_t player;
    test_conn_init(&player_conn, &player, 22);
    int32_t stone_id = mc_minecraft_item_id("minecraft:stone");
    assert(stone_id >= 0);
    mc_slot_t stack = {0};
    assert(mc_slot_set_simple(&stack, stone_id, 7) == 0);

    player_conn.gamemode = GAMEMODE_SURVIVAL;
    assert(proto_play_set_creative_slot_for_test(&player_conn, 36, &stack) == 0);
    assert(!player.inventory.slots[36].present);

    player_conn.gamemode = GAMEMODE_CREATIVE;
    assert(proto_play_set_creative_slot_for_test(&player_conn, 36, &stack) == 0);
    assert(player.inventory.slots[36].present);
    assert(player.inventory.slots[36].item_id == stone_id);
    assert(player.inventory.slots[36].count == 7);

    mc_slot_clear(&stack);
    test_conn_clear(&player_conn, &player);
}

int main(void) {
    test_serverbound_play_packet_ids();
    test_interact_parse();
    test_pvp_damage_and_gates();
    test_armor_reduces_damage();
    test_shield_blocking();
    test_lifecycle_death_empty_inventory();
    test_lifecycle_death_drops_once_and_clears_inventory();
    test_lifecycle_keep_inventory();
    test_lifecycle_respawn_reset();
    test_pickup_inventory_empty_merge_full_and_spectator();
    test_manual_drop_selected_item_stack_empty_and_dead();
    test_creative_set_slot_gate();
    return 0;
}
