#include "mc_protocol.h"
#include "mc_inventory.h"
#include "mc_crafting.h"
#include "mc_furnace.h"
#include "mc_mining.h"
#include "mc_container_store.h"
#include "mc_nbt.h"
#include "mc_packed.h"
#include "mc_player_store.h"
#include "mc_util.h"
#include "generated_minecraft_ids.h"
#include "generated_registries.h"
#include "generated_block_loot.h"
#include "generated_item_food.h"
#include "generated_item_place.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define PKT_PLAY_DISCONNECT MC_PKT_PLAY_CLIENTBOUND_DISCONNECT
#define PKT_PLAY_COMMANDS MC_PKT_PLAY_CLIENTBOUND_COMMANDS
#define PKT_PLAY_CLOSE_WINDOW MC_PKT_PLAY_CLIENTBOUND_CONTAINER_CLOSE
#define PKT_PLAY_OPEN_WINDOW MC_PKT_PLAY_CLIENTBOUND_OPEN_SCREEN
#define PKT_PLAY_SPAWN_ENTITY MC_PKT_PLAY_CLIENTBOUND_ADD_ENTITY
#define PKT_PLAY_ENTITY_EVENT MC_PKT_PLAY_CLIENTBOUND_ENTITY_EVENT
#define PKT_PLAY_UNLOAD_CHUNK MC_PKT_PLAY_CLIENTBOUND_FORGET_LEVEL_CHUNK
#define PKT_PLAY_LOGIN MC_PKT_PLAY_CLIENTBOUND_LOGIN
#define PKT_PLAY_GAME_EVENT MC_PKT_PLAY_CLIENTBOUND_GAME_EVENT
#define PKT_PLAY_RESPAWN MC_PKT_PLAY_CLIENTBOUND_RESPAWN
#define PKT_PLAY_CHUNK_DATA MC_PKT_PLAY_CLIENTBOUND_LEVEL_CHUNK_WITH_LIGHT
#define PKT_PLAY_KEEPALIVE MC_PKT_PLAY_CLIENTBOUND_KEEP_ALIVE
#define PKT_PLAY_SET_HEALTH MC_PKT_PLAY_CLIENTBOUND_SET_HEALTH
#define PKT_PLAY_ENTITY_METADATA MC_PKT_PLAY_CLIENTBOUND_SET_ENTITY_DATA
#define PKT_PLAY_SYNC_POS MC_PKT_PLAY_CLIENTBOUND_PLAYER_POSITION
#define PKT_PLAY_PLAYER_ABILITIES MC_PKT_PLAY_CLIENTBOUND_PLAYER_ABILITIES
#define PKT_PLAY_PLAYER_COMBAT_KILL MC_PKT_PLAY_CLIENTBOUND_PLAYER_COMBAT_KILL
#define PKT_PLAY_SET_CENTER_CHUNK MC_PKT_PLAY_CLIENTBOUND_SET_CHUNK_CACHE_CENTER
#define PKT_PLAY_SET_DEFAULT_SPAWN MC_PKT_PLAY_CLIENTBOUND_SET_DEFAULT_SPAWN_POSITION
#define PKT_PLAY_WINDOW_ITEMS MC_PKT_PLAY_CLIENTBOUND_CONTAINER_SET_CONTENT
#define PKT_PLAY_REL_ENTITY_MOVE MC_PKT_PLAY_CLIENTBOUND_MOVE_ENTITY_POS
#define PKT_PLAY_ENTITY_MOVE_LOOK MC_PKT_PLAY_CLIENTBOUND_MOVE_ENTITY_POS_ROT
#define PKT_PLAY_ENTITY_LOOK MC_PKT_PLAY_CLIENTBOUND_MOVE_ENTITY_ROT
#define PKT_PLAY_PLAYER_REMOVE MC_PKT_PLAY_CLIENTBOUND_PLAYER_INFO_REMOVE
#define PKT_PLAY_PLAYER_INFO MC_PKT_PLAY_CLIENTBOUND_PLAYER_INFO_UPDATE
#define PKT_PLAY_KEEPALIVE_SB MC_PKT_PLAY_SERVERBOUND_KEEP_ALIVE
#define PKT_PLAY_CONFIRM_TELEPORT MC_PKT_PLAY_SERVERBOUND_ACCEPT_TELEPORTATION
#define PKT_PLAY_BLOCK_CHANGED_ACK MC_PKT_PLAY_CLIENTBOUND_BLOCK_CHANGED_ACK
#define PKT_PLAY_BLOCK_UPDATE MC_PKT_PLAY_CLIENTBOUND_BLOCK_UPDATE
#define PKT_PLAY_BLOCK_EVENT MC_PKT_PLAY_CLIENTBOUND_BLOCK_EVENT
#define PKT_PLAY_TILE_ENTITY_DATA MC_PKT_PLAY_CLIENTBOUND_BLOCK_ENTITY_DATA
#define PKT_PLAY_SET_SLOT MC_PKT_PLAY_CLIENTBOUND_CONTAINER_SET_SLOT
#define PKT_PLAY_CONTAINER_SET_DATA MC_PKT_PLAY_CLIENTBOUND_CONTAINER_SET_DATA
#define PKT_PLAY_CHAT_COMMAND MC_PKT_PLAY_SERVERBOUND_CHAT_COMMAND
#define PKT_PLAY_SIGNED_CHAT_COMMAND MC_PKT_PLAY_SERVERBOUND_CHAT_COMMAND_SIGNED
#define PKT_PLAY_CLIENT_COMMAND_SB MC_PKT_PLAY_SERVERBOUND_CLIENT_COMMAND
#define PKT_PLAY_PLAYER_ACTION MC_PKT_PLAY_SERVERBOUND_PLAYER_ACTION
#define PKT_PLAY_HELD_ITEM_SLOT_SB MC_PKT_PLAY_SERVERBOUND_SET_CARRIED_ITEM
#define PKT_PLAY_WINDOW_CLICK MC_PKT_PLAY_SERVERBOUND_CONTAINER_CLICK
#define PKT_PLAY_CLOSE_WINDOW_SB MC_PKT_PLAY_SERVERBOUND_CONTAINER_CLOSE
#define PKT_PLAY_SET_CREATIVE_SLOT MC_PKT_PLAY_SERVERBOUND_SET_CREATIVE_MODE_SLOT
#define PKT_PLAY_USE_ITEM MC_PKT_PLAY_SERVERBOUND_USE_ITEM
#define PKT_PLAY_USE_ITEM_ON MC_PKT_PLAY_SERVERBOUND_USE_ITEM_ON
#define PKT_PLAY_HELD_ITEM_SLOT MC_PKT_PLAY_CLIENTBOUND_SET_HELD_SLOT
#define PKT_PLAY_ENTITY_DESTROY MC_PKT_PLAY_CLIENTBOUND_REMOVE_ENTITIES
#define PKT_PLAY_ENTITY_HEAD_ROTATION MC_PKT_PLAY_CLIENTBOUND_ROTATE_HEAD
#define PKT_PLAY_SET_PLAYER_POSITION MC_PKT_PLAY_SERVERBOUND_MOVE_PLAYER_POS
#define PKT_PLAY_SET_PLAYER_POS_ROT MC_PKT_PLAY_SERVERBOUND_MOVE_PLAYER_POS_ROT
#define PKT_PLAY_SET_PLAYER_ROT MC_PKT_PLAY_SERVERBOUND_MOVE_PLAYER_ROT
#define PKT_PLAY_SET_PLAYER_ON_GROUND MC_PKT_PLAY_SERVERBOUND_MOVE_PLAYER_STATUS_ONLY
#define PKT_PLAY_PONG_SB MC_PKT_PLAY_SERVERBOUND_PONG
#define PKT_PLAY_ENTITY_TELEPORT MC_PKT_PLAY_CLIENTBOUND_TELEPORT_ENTITY
#define PKT_PLAY_CLIENTBOUND_PING MC_PKT_PLAY_CLIENTBOUND_PING
#define PKT_PLAY_CHANGE_DIFFICULTY MC_PKT_PLAY_CLIENTBOUND_CHANGE_DIFFICULTY
#define PKT_PLAY_SYSTEM_CHAT MC_PKT_PLAY_CLIENTBOUND_SYSTEM_CHAT

#define CHUNK_XZ 16

#define CHUNKS_PER_TICK 4
#define CHUNK_SEND_SCAN_LIMIT 32

#define ENTITY_STATUS_OP_LEVEL_4 28

#define GAMEMODE_SURVIVAL 0
#define GAMEMODE_CREATIVE 1
#define GAMEMODE_ADVENTURE 2
#define GAMEMODE_SPECTATOR 3

#define KEEPALIVE_INTERVAL_MS 15000
#define KEEPALIVE_TIMEOUT_MS 20000

#define PLAYER_MAX_HEALTH 20.0f
#define PLAYER_MAX_FOOD_LEVEL 20
#define PLAYER_DEFAULT_FOOD_LEVEL PLAYER_MAX_FOOD_LEVEL
#define PLAYER_DEFAULT_FOOD_SATURATION 5.0f
#define PLAYER_MAX_FOOD_EXHAUSTION 40.0f
#define PLAYER_FOOD_EXHAUSTION_STEP 4.0f
#define PLAYER_MOVE_EXHAUSTION_PER_BLOCK 0.05f
#define PLAYER_SPRINT_EXHAUSTION_PER_BLOCK 0.15f
#define PLAYER_JUMP_EXHAUSTION 0.2f
#define PLAYER_SPRINT_DISTANCE_THRESHOLD 0.18
#define PLAYER_MAX_HUNGER_SAMPLE_DISTANCE 2.0
#define PLAYER_MIN_MOVEMENT_SAMPLE 0.001
#define PLAYER_JUMP_MIN_ASCENT 0.2
#define PLAYER_NATURAL_REGEN_FOOD_THRESHOLD 18
#define PLAYER_NATURAL_REGEN_INTERVAL_MS 4000
#define PLAYER_PEACEFUL_REGEN_INTERVAL_MS 1000
#define PLAYER_NATURAL_REGEN_AMOUNT 1.0f
#define PLAYER_NATURAL_REGEN_EXHAUSTION 6.0f
#define PLAYER_STARVATION_DAMAGE_INTERVAL_MS 4000
#define PLAYER_STARVATION_DAMAGE_AMOUNT 1.0f
#define PLAYER_MANUAL_DROP_PICKUP_DELAY_TICKS 40
#define PLAYER_FOOD_USE_DURATION_TICKS 32
#define PLAYER_DROP_FORWARD_OFFSET 0.35
#define PLAYER_DROP_VERTICAL_OFFSET 1.2
#define PLAYER_DROP_FORWARD_SPEED 0.35
#define PLAYER_DROP_UPWARD_SPEED 0.20
#define PLAYER_COLLISION_HALF_WIDTH 0.3
#define PLAYER_COLLISION_HEIGHT 1.8
#define PLAYER_COLLISION_EPSILON 1.0e-7
#define PLAYER_VOID_DAMAGE_Y ((double)MC_WORLD_MIN_Y - 0.5)
#define PLAYER_VOID_DAMAGE_AMOUNT 4.0f
#define PLAYER_VOID_DAMAGE_INTERVAL_MS 500
#define PLAYER_FALL_SAFE_DISTANCE 3.0
#define PLAYER_RESPAWN_COPY_METADATA 0
#define PLAYER_CLIENT_COMMAND_PERFORM_RESPAWN 0
#define PLAYER_ACTION_START_DESTROY_BLOCK 0
#define PLAYER_ACTION_ABORT_DESTROY_BLOCK 1
#define PLAYER_ACTION_STOP_DESTROY_BLOCK 2
#define PLAYER_ACTION_DROP_ALL_ITEMS 3
#define PLAYER_ACTION_DROP_ITEM 4
#define PLAYER_ACTION_RELEASE_USE_ITEM 5
#define WORLD_SPAWN_X 0.5
#define WORLD_SPAWN_Y 64.0
#define WORLD_SPAWN_Z 0.5
#define WORLD_SPAWN_YAW 0.0f
#define WORLD_SPAWN_PITCH 0.0f

#define PLAYER_METADATA_LIVING_FLAGS_INDEX 8
#define ENTITY_METADATA_TYPE_BYTE 0
#define LIVING_ENTITY_FLAG_USING_ITEM 0x01
#define LIVING_ENTITY_FLAG_USING_OFFHAND 0x02

#define PLAYER_INFO_ACTION_ADD 0x01
#define PLAYER_INFO_ACTION_UPDATE_GAMEMODE 0x04
#define PLAYER_INFO_ACTION_UPDATE_LISTED 0x08
#define PLAYER_INFO_ACTION_UPDATE_LATENCY 0x10

#define MC_WINDOW_TYPE_GENERIC_9X3 2
#define MC_WINDOW_TYPE_CRAFTING 12
#define MC_WINDOW_TYPE_BLAST_FURNACE 10
#define MC_WINDOW_TYPE_FURNACE 14
#define MC_WINDOW_TYPE_SMOKER 22
#define MC_CONTAINER_INPUT_PICKUP 0
#define MC_CONTAINER_INPUT_QUICK_MOVE 1
#define MC_CONTAINER_INPUT_THROW 4
#define PLAYER_CRAFTING_RESULT_SLOT 0
#define PLAYER_CRAFTING_GRID_SLOT 1
#define PLAYER_CRAFTING_GRID_WIDTH 2
#define PLAYER_CRAFTING_GRID_HEIGHT 2
#define CRAFTING_TABLE_RESULT_SLOT 0
#define CRAFTING_TABLE_GRID_SLOT 1
#define CRAFTING_TABLE_GRID_WIDTH 3
#define CRAFTING_TABLE_GRID_HEIGHT 3
#define CRAFTING_TABLE_SLOT_COUNT 10

#define ITEM_AIR 0
#define CHUNK_REFRESH_PING_ID 0x0000CAFE

static mc_world_t *get_world(mc_conn_t *c);
static int buf_w_u8(mc_buf_t *b, uint8_t v);
static int buf_w_u16_be(mc_buf_t *b, uint16_t v);
static int buf_w_varint(mc_buf_t *b, int32_t v);
static void close_active_window(mc_conn_t *c, bool notify_client);
static void sent_chunks_clear(mc_conn_t *c);
static void pending_chunks_clear(mc_conn_t *c);
static int container_block_entity_type(int32_t state_id);
static const char *block_entity_type_name_for_state(int32_t state_id);
static bool is_chest_state(int32_t state_id);
static bool is_trapped_chest_state(int32_t state_id);
static bool is_ender_chest_state(int32_t state_id);
static bool is_shulker_box_state(int32_t state_id);
static bool is_crafting_table_state(int32_t state_id);
static bool is_furnace_like_state(int32_t state_id);
static mc_container_kind_t container_kind_for_state(int32_t state_id);
static mc_block_entity_type_t container_entity_type_for_state(int32_t state_id);
static int block_entity_from_container_instance(mc_block_entity_t *dst, mc_block_entity_type_t type, const mc_container_instance_t *src);
static int sync_active_container_window(mc_conn_t *c);
static int send_chunk_ready(mc_conn_t *c, mc_world_t *world, int32_t cx, int32_t cz, const mc_chunk_t *chunk);
static int paletted_word_count_for_network(int entry_count, int bits);
static int encode_paletted_container_network(mc_buf_t *out, const uint32_t *values, int entry_count, int palette_bits);
static int32_t mc_resolve_placement_state(const mc_slot_t *slot, float player_yaw, int32_t clicked_face);
static int send_block_entity_data_packet(mc_conn_t *c, int32_t state_id, int32_t x, int32_t y, int32_t z);
static int send_block_event_packet(mc_conn_t *c, int32_t x, int32_t y, int32_t z, uint8_t action, uint8_t param, int32_t block_id);
static int send_sent_chunks_post_updates(mc_conn_t *c);
static int maybe_send_chunk_refresh_ping(mc_conn_t *c);
static int send_set_health_packet(mc_conn_t *c);
static int send_window_items(mc_conn_t *c);
static int sync_inventory_full(mc_conn_t *c);
static int respawn_player(mc_conn_t *c);
static int apply_player_damage(mc_conn_t *c, float amount, const char *death_message, bool server_locked, int64_t now_ms);
static int make_text_component(const char *text, uint8_t **out, size_t *out_len);
static void reset_fall_tracking(mc_conn_t *c);
static int try_consume_selected_food(mc_conn_t *c);
static int drop_selected_mainhand_items(mc_conn_t *c, int32_t requested_count);
static int drop_player_slot(mc_conn_t *c, mc_slot_t *slot, bool server_locked);
static int player_sync_food_state(mc_conn_t *c, bool persist);
static void clear_active_item_use(mc_conn_t *c);
static void cancel_item_use(mc_conn_t *c);
static int tick_item_use(mc_conn_t *c);
static int update_player_crafting_result(mc_conn_t *c);
static int return_player_crafting_grid(mc_conn_t *c);
static int return_container_crafting_grid(mc_conn_t *c, mc_container_instance_t *container);

static int w_varint(uint8_t *buf, size_t cap, size_t *pos, int32_t v) {
    size_t n = 0;
    if (varint_write(buf + *pos, cap - *pos, v, &n) != 0) return -1;
    *pos += n;
    return 0;
}

static int w_string(uint8_t *buf, size_t cap, size_t *pos, const char *s) {
    size_t len = strlen(s);
    if (w_varint(buf, cap, pos, (int32_t)len) != 0) return -1;
    if (*pos + len > cap) return -1;
    memcpy(buf + *pos, s, len);
    *pos += len;
    return 0;
}

static int w_bool(uint8_t *buf, size_t cap, size_t *pos, bool v) {
    if (*pos + 1 > cap) return -1;
    buf[*pos] = v ? 0x01 : 0x00;
    *pos += 1;
    return 0;
}

static int w_byte(uint8_t *buf, size_t cap, size_t *pos, int8_t v) {
    if (*pos + 1 > cap) return -1;
    buf[*pos] = (uint8_t)v;
    *pos += 1;
    return 0;
}

static int w_ubyte(uint8_t *buf, size_t cap, size_t *pos, uint8_t v) {
    if (*pos + 1 > cap) return -1;
    buf[*pos] = v;
    *pos += 1;
    return 0;
}

static int w_u32(uint8_t *buf, size_t cap, size_t *pos, uint32_t v) {
    if (*pos + 4 > cap) return -1;
    buf[*pos + 0] = (uint8_t)((v >> 24) & 0xFF);
    buf[*pos + 1] = (uint8_t)((v >> 16) & 0xFF);
    buf[*pos + 2] = (uint8_t)((v >> 8) & 0xFF);
    buf[*pos + 3] = (uint8_t)(v & 0xFF);
    *pos += 4;
    return 0;
}

static int w_u16_be(uint8_t *buf, size_t cap, size_t *pos, uint16_t v) {
    if (*pos + 2 > cap) return -1;
    buf[*pos + 0] = (uint8_t)((v >> 8) & 0xFF);
    buf[*pos + 1] = (uint8_t)(v & 0xFF);
    *pos += 2;
    return 0;
}

static int w_lpvec3_zero(uint8_t *buf, size_t cap, size_t *pos) {
    return w_ubyte(buf, cap, pos, 0);
}

static int w_u64(uint8_t *buf, size_t cap, size_t *pos, uint64_t v) {
    if (*pos + 8 > cap) return -1;
    buf[*pos + 0] = (uint8_t)((v >> 56) & 0xFF);
    buf[*pos + 1] = (uint8_t)((v >> 48) & 0xFF);
    buf[*pos + 2] = (uint8_t)((v >> 40) & 0xFF);
    buf[*pos + 3] = (uint8_t)((v >> 32) & 0xFF);
    buf[*pos + 4] = (uint8_t)((v >> 24) & 0xFF);
    buf[*pos + 5] = (uint8_t)((v >> 16) & 0xFF);
    buf[*pos + 6] = (uint8_t)((v >> 8) & 0xFF);
    buf[*pos + 7] = (uint8_t)(v & 0xFF);
    *pos += 8;
    return 0;
}

static int w_i32(uint8_t *buf, size_t cap, size_t *pos, int32_t v) {
    return w_u32(buf, cap, pos, (uint32_t)v);
}

static int w_i64(uint8_t *buf, size_t cap, size_t *pos, int64_t v) {
    return w_u64(buf, cap, pos, (uint64_t)v);
}

static int w_f32(uint8_t *buf, size_t cap, size_t *pos, float v) {
    uint32_t u = 0;
    memcpy(&u, &v, sizeof(u));
    return w_u32(buf, cap, pos, u);
}

static int w_f64(uint8_t *buf, size_t cap, size_t *pos, double v) {
    uint64_t u = 0;
    memcpy(&u, &v, sizeof(u));
    return w_u64(buf, cap, pos, u);
}

static int w_position(uint8_t *buf, size_t cap, size_t *pos, int32_t x, int32_t y, int32_t z) {
    uint64_t packed = ((uint64_t)(x & 0x3FFFFFF) << 38) | ((uint64_t)(z & 0x3FFFFFF) << 12) | ((uint64_t)(y & 0xFFF));
    return w_u64(buf, cap, pos, packed);
}

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} mc_reader_t;

static int r_varint(mc_reader_t *r, int32_t *out) {
    size_t n = 0;
    if (r->pos >= r->len) return -1;
    if (varint_read(r->data + r->pos, r->len - r->pos, out, &n) != 0) return -1;
    r->pos += n;
    return 0;
}

static int r_string_alloc(mc_reader_t *r, char **out) {
    int32_t len = 0;
    if (r_varint(r, &len) != 0) return -1;
    if (len < 0 || (size_t)len > r->len - r->pos) return -1;
    char *s = (char *)malloc((size_t)len + 1);
    if (!s) return -1;
    memcpy(s, r->data + r->pos, (size_t)len);
    s[len] = '\0';
    r->pos += (size_t)len;
    *out = s;
    return 0;
}

static int r_bool(mc_reader_t *r, bool *out) {
    if (r->pos + 1 > r->len) return -1;
    *out = r->data[r->pos++] ? true : false;
    return 0;
}

static int r_f32(mc_reader_t *r, float *out) {
    if (r->pos + 4 > r->len) return -1;
    uint32_t u = 0;
    u |= (uint32_t)r->data[r->pos + 0] << 24;
    u |= (uint32_t)r->data[r->pos + 1] << 16;
    u |= (uint32_t)r->data[r->pos + 2] << 8;
    u |= (uint32_t)r->data[r->pos + 3];
    r->pos += 4;
    memcpy(out, &u, sizeof(u));
    return 0;
}

static int r_i16(mc_reader_t *r, int16_t *out) {
    if (!r || !out || r->pos + 2 > r->len) return -1;
    uint16_t v = ((uint16_t)r->data[r->pos] << 8) | (uint16_t)r->data[r->pos + 1];
    r->pos += 2;
    *out = (int16_t)v;
    return 0;
}

static int r_i32(mc_reader_t *r, int32_t *out) {
    if (!r || !out || r->pos + 4 > r->len) return -1;
    uint32_t v = ((uint32_t)r->data[r->pos + 0] << 24) |
                 ((uint32_t)r->data[r->pos + 1] << 16) |
                 ((uint32_t)r->data[r->pos + 2] << 8) |
                 (uint32_t)r->data[r->pos + 3];
    r->pos += 4;
    *out = (int32_t)v;
    return 0;
}

static int r_f64(mc_reader_t *r, double *out) {
    if (r->pos + 8 > r->len) return -1;
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) {
        u = (u << 8) | r->data[r->pos + i];
    }
    r->pos += 8;
    memcpy(out, &u, sizeof(u));
    return 0;
}

static int r_hashed_patch_map_skip(mc_reader_t *r) {
    int32_t added_count = 0;
    int32_t removed_count = 0;
    if (r_varint(r, &added_count) != 0 || r_varint(r, &removed_count) != 0) return -1;
    if (added_count < 0 || removed_count < 0) return -1;
    for (int32_t i = 0; i < added_count; i++) {
        int32_t component_type_id = 0;
        int32_t component_hash = 0;
        if (r_varint(r, &component_type_id) != 0) return -1;
        if (r_i32(r, &component_hash) != 0) return -1;
    }
    for (int32_t i = 0; i < removed_count; i++) {
        int32_t component_type_id = 0;
        if (r_varint(r, &component_type_id) != 0) return -1;
    }
    return 0;
}

static int r_hashed_stack(mc_reader_t *r, mc_slot_t *out) {
    if (!r || !out) return -1;
    bool present = false;
    if (r_bool(r, &present) != 0) return -1;
    if (!present) {
        mc_slot_clear(out);
        return 0;
    }

    int32_t item_id = 0;
    int32_t count = 0;
    if (r_varint(r, &item_id) != 0) return -1;
    if (r_varint(r, &count) != 0) return -1;
    if (r_hashed_patch_map_skip(r) != 0) return -1;

    mc_slot_clear(out);
    if (item_id <= 0 || count <= 0) return 0;
    out->present = true;
    out->item_id = item_id;
    out->count = count;
    return 0;
}

static int r_position(mc_reader_t *r, int32_t *x, int32_t *y, int32_t *z) {
    if (r->pos + 8 > r->len) return -1;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | r->data[r->pos + i];
    }
    r->pos += 8;
    int32_t rx = (int32_t)(v >> 38);
    int32_t rz = (int32_t)((v >> 12) & 0x3FFFFFF);
    int32_t ry = (int32_t)(v & 0xFFF);
    if (rx >= (1 << 25)) rx -= (1 << 26);
    if (rz >= (1 << 25)) rz -= (1 << 26);
    if (ry >= (1 << 11)) ry -= (1 << 12);
    *x = rx;
    *y = ry;
    *z = rz;
    return 0;
}

static mc_world_t *get_world(mc_conn_t *c) {
    return (c && c->server) ? net_server_world(c->server) : NULL;
}

static mc_difficulty_t current_difficulty(const mc_conn_t *c) {
    return (c && c->server) ? net_server_get_difficulty(c->server) : MC_DIFFICULTY_NORMAL;
}

static bool parse_difficulty_text(const char *text, mc_difficulty_t *out) {
    if (!text || !out) return false;

    char tmp[32];
    size_t n = strlen(text);
    if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
    for (size_t i = 0; i < n; i++) tmp[i] = (char)tolower((unsigned char)text[i]);
    tmp[n] = '\0';

    if (strcmp(tmp, "0") == 0 || strcmp(tmp, "p") == 0 || strcmp(tmp, "peaceful") == 0 || strcmp(tmp, "paisible") == 0) {
        *out = MC_DIFFICULTY_PEACEFUL;
        return true;
    }
    if (strcmp(tmp, "1") == 0 || strcmp(tmp, "e") == 0 || strcmp(tmp, "easy") == 0 || strcmp(tmp, "facile") == 0) {
        *out = MC_DIFFICULTY_EASY;
        return true;
    }
    if (strcmp(tmp, "2") == 0 || strcmp(tmp, "n") == 0 || strcmp(tmp, "normal") == 0 || strcmp(tmp, "normale") == 0) {
        *out = MC_DIFFICULTY_NORMAL;
        return true;
    }
    if (strcmp(tmp, "3") == 0 || strcmp(tmp, "h") == 0 || strcmp(tmp, "hard") == 0 || strcmp(tmp, "difficile") == 0) {
        *out = MC_DIFFICULTY_HARD;
        return true;
    }
    return false;
}

static int32_t clamp_food_level(int32_t food_level) {
    if (food_level < 0) return 0;
    if (food_level > PLAYER_MAX_FOOD_LEVEL) return PLAYER_MAX_FOOD_LEVEL;
    return food_level;
}

static float clamp_food_exhaustion(float exhaustion) {
    if (!isfinite(exhaustion) || exhaustion < 0.0f) return 0.0f;
    if (exhaustion > PLAYER_MAX_FOOD_EXHAUSTION) return PLAYER_MAX_FOOD_EXHAUSTION;
    return exhaustion;
}

static float clamp_food_saturation(int32_t food_level, float saturation) {
    float max_saturation = (float)clamp_food_level(food_level);
    if (saturation < 0.0f) return 0.0f;
    if (saturation > max_saturation) return max_saturation;
    return saturation;
}

static float clamp_player_health(float health) {
    if (!isfinite(health)) return PLAYER_MAX_HEALTH;
    if (health < 0.0f) return 0.0f;
    if (health > PLAYER_MAX_HEALTH) return PLAYER_MAX_HEALTH;
    return health;
}

static int save_player_data(mc_conn_t *c) {
    mc_world_t *world = get_world(c);
    if (!c || !c->player || !world) return 0;
    const char *world_path = mc_world_path(world);
    if (!world_path || !*world_path) return 0;
    c->player->health = c->health;
    c->food = clamp_food_level(c->food);
    c->food_saturation = clamp_food_saturation(c->food, c->food_saturation);
    c->food_exhaustion = clamp_food_exhaustion(c->food_exhaustion);
    c->player->food_level = c->food;
    c->player->food_saturation = c->food_saturation;
    c->player->food_exhaustion = c->food_exhaustion;
    c->player->pos_x = c->x;
    c->player->pos_y = c->y;
    c->player->pos_z = c->z;
    c->player->yaw = c->yaw;
    c->player->pitch = c->pitch;
    c->player->gamemode = c->gamemode;
    if (mc_player_store_save(world_path, c->player) != 0) {
        log_error("player save failed: user=%s world=%s", c->player->username, world_path);
        return -1;
    }
    return 0;
}

static int ensure_player_loaded(mc_conn_t *c) {
    if (!c) return -1;
    if (c->player) return 0;

    mc_world_t *world = get_world(c);
    if (!world) return -1;
    const char *world_path = mc_world_path(world);

    mc_player_data_t *player = (mc_player_data_t *)calloc(1, sizeof(*player));
    if (!player) return -1;
    mc_player_data_init(player);
    player->has_uuid = c->has_uuid;
    memcpy(player->uuid, c->uuid, sizeof(player->uuid));
    snprintf(player->username, sizeof(player->username), "%s", c->username);
    player->gamemode = GAMEMODE_CREATIVE;

    int rc = 1;
    if (world_path && *world_path) {
        rc = mc_player_store_load(world_path, c->uuid, c->has_uuid, c->username, player);
        if (rc < 0) {
            log_error("player load failed: user=%s world=%s", c->username, world_path);
            mc_player_data_clear(player);
            free(player);
            return -1;
        }
    }
    if (rc == 1) {
        mc_inventory_fill_starter_loadout(&player->inventory);
        player->health = PLAYER_MAX_HEALTH;
        player->food_level = PLAYER_DEFAULT_FOOD_LEVEL;
        player->food_saturation = PLAYER_DEFAULT_FOOD_SATURATION;
        player->pos_x = WORLD_SPAWN_X;
        player->pos_y = WORLD_SPAWN_Y;
        player->pos_z = WORLD_SPAWN_Z;
        player->yaw = WORLD_SPAWN_YAW;
        player->pitch = WORLD_SPAWN_PITCH;
    }
    if (player->health <= 0.0f) {
        player->health = PLAYER_MAX_HEALTH;
        player->pos_x = WORLD_SPAWN_X;
        player->pos_y = WORLD_SPAWN_Y;
        player->pos_z = WORLD_SPAWN_Z;
        player->yaw = WORLD_SPAWN_YAW;
        player->pitch = WORLD_SPAWN_PITCH;
    }
    if (!isfinite(player->pos_x) || !isfinite(player->pos_y) || !isfinite(player->pos_z) ||
        player->pos_y < (double)MC_WORLD_MIN_Y || player->pos_y > (double)(MC_WORLD_MIN_Y + MC_WORLD_HEIGHT + 256)) {
        log_error("player load position invalid: user=%s pos=(%.3f,%.3f,%.3f); resetting to spawn",
                  c->username[0] ? c->username : "(unknown)", player->pos_x, player->pos_y, player->pos_z);
        player->pos_x = WORLD_SPAWN_X;
        player->pos_y = WORLD_SPAWN_Y;
        player->pos_z = WORLD_SPAWN_Z;
        player->yaw = WORLD_SPAWN_YAW;
        player->pitch = WORLD_SPAWN_PITCH;
    }
    player->food_level = clamp_food_level(player->food_level);
    player->food_saturation = clamp_food_saturation(player->food_level, player->food_saturation);
    player->food_exhaustion = clamp_food_exhaustion(player->food_exhaustion);
    c->gamemode = player->gamemode;
    c->health = player->health;
    c->food = player->food_level;
    c->food_saturation = player->food_saturation;
    c->food_exhaustion = player->food_exhaustion;
    c->dead = false;
    c->on_ground = false;
    c->fall_tracking = false;
    c->fall_start_y = player->pos_y;
    c->next_void_damage_ms = 0;
    c->next_natural_regen_ms = 0;
    c->next_starvation_damage_ms = 0;
    c->player = player;
    return 0;
}

void proto_play_conn_cleanup(mc_conn_t *c) {
    if (!c) return;

    cancel_item_use(c);
    close_active_window(c, false);
    if (c->player) {
        (void)return_player_crafting_grid(c);
        (void)save_player_data(c);
        mc_player_data_clear(c->player);
        free(c->player);
        c->player = NULL;
    }

    c->play_init_sent = false;
    c->play_ready = false;
    c->teleport_id = 0;
    c->has_pos = false;
    c->has_center_chunk = false;
    c->center_cx = 0;
    c->center_cz = 0;
    c->chunk_refresh_ping_pending = false;
    c->chunk_refresh_ping_id = 0;
    c->awaiting_keepalive = false;
    c->keepalive_id = 0;
    c->last_keepalive_sent_ms = 0;
    c->health = 0.0f;
    c->food = 0;
    c->food_saturation = 0.0f;
    c->food_exhaustion = 0.0f;
    c->dead = false;
    c->on_ground = false;
    c->fall_tracking = false;
    c->fall_start_y = 0.0;
    c->next_void_damage_ms = 0;
    c->next_natural_regen_ms = 0;
    c->next_starvation_damage_ms = 0;
    c->remote_players_len = 0;
    sent_chunks_clear(c);
    pending_chunks_clear(c);
}

static int send_game_mode_event(mc_conn_t *c, int32_t gamemode) {
    uint8_t buf[8];
    size_t pos = 0;
    if (w_ubyte(buf, sizeof(buf), &pos, 3) != 0) return -1; /* change game mode */
    if (w_f32(buf, sizeof(buf), &pos, (float)gamemode) != 0) return -1;
    return conn_write_packet(c, PKT_PLAY_GAME_EVENT, buf, pos, -1);
}

static int send_entity_event(mc_conn_t *c, uint8_t status) {
    uint8_t buf[8];
    size_t pos = 0;
    if (w_i32(buf, sizeof(buf), &pos, c->entity_id) != 0) return -1;
    if (w_byte(buf, sizeof(buf), &pos, (int8_t)status) != 0) return -1;
    return conn_write_packet(c, PKT_PLAY_ENTITY_EVENT, buf, pos, -1);
}

static int send_system_message(mc_conn_t *c, const char *text) {
    if (!c) return -1;
    uint8_t *msg = NULL;
    size_t msg_len = 0;
    if (make_text_component(text ? text : "", &msg, &msg_len) != 0) return -1;

    mc_buf_t payload;
    if (buf_init(&payload, msg_len + 8) != 0) {
        free(msg);
        return -1;
    }
    int rc = -1;
    if (buf_write(&payload, msg, msg_len) != 0) goto done;
    if (buf_w_u8(&payload, 0) != 0) goto done; /* overlay=false: chat/system line */
    rc = conn_write_packet(c, PKT_PLAY_SYSTEM_CHAT, payload.data, payload.len, -1);

done:
    buf_free(&payload);
    free(msg);
    return rc;
}

static int send_change_difficulty_packet(mc_conn_t *c, mc_difficulty_t difficulty) {
    if (!c) return -1;
    uint8_t buf[8];
    size_t pos = 0;
    if (w_varint(buf, sizeof(buf), &pos, (int32_t)difficulty) != 0) return -1;
    if (w_bool(buf, sizeof(buf), &pos, false) != 0) return -1; /* locked=false */
    return conn_write_packet(c, PKT_PLAY_CHANGE_DIFFICULTY, buf, pos, -1);
}

int proto_play_send_difficulty(mc_conn_t *c) {
    if (!c) return -1;
    return send_change_difficulty_packet(c, current_difficulty(c));
}

static int send_player_abilities(mc_conn_t *c) {
    uint8_t buf[32];
    size_t pos = 0;
    uint8_t flags = 0;
    if (c->gamemode == GAMEMODE_CREATIVE) {
        flags |= 0x01; /* invulnerable */
        flags |= 0x02; /* flying */
        flags |= 0x04; /* allow flying */
        flags |= 0x08; /* creative mode */
    } else if (c->gamemode == GAMEMODE_SPECTATOR) {
        flags |= 0x01; /* invulnerable */
        flags |= 0x02; /* flying */
        flags |= 0x04; /* allow flying */
    }
    if (w_ubyte(buf, sizeof(buf), &pos, flags) != 0) return -1;
    if (w_f32(buf, sizeof(buf), &pos, 0.05f) != 0) return -1; /* flying speed */
    if (w_f32(buf, sizeof(buf), &pos, 0.1f) != 0) return -1;  /* walking speed */
    return conn_write_packet(c, PKT_PLAY_PLAYER_ABILITIES, buf, pos, -1);
}

static int send_sync_position(mc_conn_t *c, double x, double y, double z, float yaw, float pitch) {
    uint8_t buf[256];
    size_t pos = 0;
    c->teleport_id += 1;
    if (w_varint(buf, sizeof(buf), &pos, c->teleport_id) != 0) return -1;

    /* 26.1 ClientboundPlayerPositionPacket = teleport id + PositionMoveRotation + Relative set.
     * PositionMoveRotation carries absolute position, absolute delta movement, then yaw/pitch. */
    if (w_f64(buf, sizeof(buf), &pos, x) != 0) return -1;
    if (w_f64(buf, sizeof(buf), &pos, y) != 0) return -1;
    if (w_f64(buf, sizeof(buf), &pos, z) != 0) return -1;
    if (w_f64(buf, sizeof(buf), &pos, 0.0) != 0) return -1;
    if (w_f64(buf, sizeof(buf), &pos, 0.0) != 0) return -1;
    if (w_f64(buf, sizeof(buf), &pos, 0.0) != 0) return -1;
    if (w_f32(buf, sizeof(buf), &pos, yaw) != 0) return -1;
    if (w_f32(buf, sizeof(buf), &pos, pitch) != 0) return -1;
    if (w_i32(buf, sizeof(buf), &pos, 0) != 0) return -1; /* empty Relative set bitmask */
    if (conn_write_packet(c, PKT_PLAY_SYNC_POS, buf, pos, -1) != 0) return -1;
    c->x = x;
    c->y = y;
    c->z = z;
    c->yaw = yaw;
    c->pitch = pitch;
    c->has_pos = true;
    reset_fall_tracking(c);
    return 0;
}

typedef enum {
    CMD_PROP_NONE = 0,
    CMD_PROP_ENTITY_FLAGS,
    CMD_PROP_STRING_BEHAVIOR
} cmd_prop_kind_t;

static int commands_write_node(uint8_t *buf, size_t cap, size_t *pos, uint8_t flags,
                               const int *children, size_t child_count,
                               const char *name, int32_t parser_id,
                               cmd_prop_kind_t prop_kind, int32_t prop_value) {
    if (w_ubyte(buf, cap, pos, flags) != 0) return -1;
    if (w_varint(buf, cap, pos, (int32_t)child_count) != 0) return -1;
    for (size_t i = 0; i < child_count; i++) {
        if (w_varint(buf, cap, pos, children[i]) != 0) return -1;
    }
    if (flags & 0x08) {
        if (w_varint(buf, cap, pos, 0) != 0) return -1;
    }
    if ((flags & 0x03) == 0x01) {
        if (w_string(buf, cap, pos, name) != 0) return -1;
    } else if ((flags & 0x03) == 0x02) {
        if (w_string(buf, cap, pos, name) != 0) return -1;
        if (w_varint(buf, cap, pos, parser_id) != 0) return -1;
        if (prop_kind == CMD_PROP_ENTITY_FLAGS) {
            if (w_ubyte(buf, cap, pos, (uint8_t)prop_value) != 0) return -1;
        } else if (prop_kind == CMD_PROP_STRING_BEHAVIOR) {
            if (w_varint(buf, cap, pos, prop_value) != 0) return -1;
        }
    }
    if (flags & 0x10) {
        if (w_string(buf, cap, pos, "minecraft:ask_server") != 0) return -1;
    }
    return 0;
}

static int send_commands(mc_conn_t *c) {
    uint8_t buf[8192];
    size_t pos = 0;

    const int node_count = 32;
    if (w_varint(buf, sizeof(buf), &pos, node_count) != 0) return -1;

    /* Parser IDs (1.19+): brigadier:string=5, minecraft:entity=6, minecraft:vec3=10 */
    const int32_t PARSER_STRING = 5;
    const int32_t PARSER_ENTITY = 6;
    const int32_t PARSER_VEC3 = 10;

    /* 0: argument mode (gamemode) using brigadier:string SINGLE_WORD */
    if (commands_write_node(buf, sizeof(buf), &pos, 0x06, NULL, 0,
                            "mode", PARSER_STRING, CMD_PROP_STRING_BEHAVIOR, 0) != 0) return -1;

    /* 1: argument location (vec3) */
    if (commands_write_node(buf, sizeof(buf), &pos, 0x06, NULL, 0,
                            "location", PARSER_VEC3, CMD_PROP_NONE, 0) != 0) return -1;

    /* 2: argument destination (entity) */
    if (commands_write_node(buf, sizeof(buf), &pos, 0x06, NULL, 0,
                            "destination", PARSER_ENTITY, CMD_PROP_ENTITY_FLAGS, 0x03) != 0) return -1;

    /* 3: argument location (vec3) after target */
    if (commands_write_node(buf, sizeof(buf), &pos, 0x06, NULL, 0,
                            "location", PARSER_VEC3, CMD_PROP_NONE, 0) != 0) return -1;

    /* 4: argument target (entity) -> [location|destination] */
    {
        const int children[] = {3, 2};
        if (commands_write_node(buf, sizeof(buf), &pos, 0x02, children, 2,
                                "target", PARSER_ENTITY, CMD_PROP_ENTITY_FLAGS, 0x03) != 0) return -1;
    }

    /* 5: literal gamemode -> mode */
    {
        const int children[] = {0};
        if (commands_write_node(buf, sizeof(buf), &pos, 0x01, children, 1,
                                "gamemode", 0, CMD_PROP_NONE, 0) != 0) return -1;
    }

    /* 6: literal tp -> [location|target] */
    {
        const int children[] = {1, 4};
        if (commands_write_node(buf, sizeof(buf), &pos, 0x01, children, 2,
                                "tp", 0, CMD_PROP_NONE, 0) != 0) return -1;
    }

    /* 7: argument block (string SINGLE_WORD) */
    if (commands_write_node(buf, sizeof(buf), &pos, 0x06, NULL, 0,
                            "block", PARSER_STRING, CMD_PROP_STRING_BEHAVIOR, 0) != 0) return -1;

    /* 8: argument location (vec3) -> block */
    {
        const int children[] = {7};
        if (commands_write_node(buf, sizeof(buf), &pos, 0x02, children, 1,
                                "location", PARSER_VEC3, CMD_PROP_NONE, 0) != 0) return -1;
    }

    /* 9: literal setblock -> location */
    {
        const int children[] = {8};
        if (commands_write_node(buf, sizeof(buf), &pos, 0x01, children, 1,
                                "setblock", 0, CMD_PROP_NONE, 0) != 0) return -1;
    }

    /* 10: generic value argument for debug commands */
    if (commands_write_node(buf, sizeof(buf), &pos, 0x06, NULL, 0,
                            "value", PARSER_STRING, CMD_PROP_STRING_BEHAVIOR, 0) != 0) return -1;

    /* 11-14: food subcommands -> value */
    {
        const int children[] = {10};
        if (commands_write_node(buf, sizeof(buf), &pos, 0x01, children, 1,
                                "set", 0, CMD_PROP_NONE, 0) != 0) return -1;
        if (commands_write_node(buf, sizeof(buf), &pos, 0x01, children, 1,
                                "add", 0, CMD_PROP_NONE, 0) != 0) return -1;
        if (commands_write_node(buf, sizeof(buf), &pos, 0x01, children, 1,
                                "sat", 0, CMD_PROP_NONE, 0) != 0) return -1;
        if (commands_write_node(buf, sizeof(buf), &pos, 0x01, children, 1,
                                "exhaust", 0, CMD_PROP_NONE, 0) != 0) return -1;
    }

    /* 15: literal food -> [set|add|sat|exhaust] */
    {
        const int children[] = {11, 12, 13, 14};
        if (commands_write_node(buf, sizeof(buf), &pos, 0x01, children, 4,
                                "food", 0, CMD_PROP_NONE, 0) != 0) return -1;
    }

    /* 16-21: health subcommands */
    {
        const int value_child[] = {10};
        if (commands_write_node(buf, sizeof(buf), &pos, 0x01, value_child, 1,
                                "set", 0, CMD_PROP_NONE, 0) != 0) return -1;
        if (commands_write_node(buf, sizeof(buf), &pos, 0x01, value_child, 1,
                                "add", 0, CMD_PROP_NONE, 0) != 0) return -1;
        if (commands_write_node(buf, sizeof(buf), &pos, 0x01, value_child, 1,
                                "damage", 0, CMD_PROP_NONE, 0) != 0) return -1;
        if (commands_write_node(buf, sizeof(buf), &pos, 0x01, value_child, 1,
                                "dmg", 0, CMD_PROP_NONE, 0) != 0) return -1;
        if (commands_write_node(buf, sizeof(buf), &pos, 0x01, value_child, 1,
                                "heal", 0, CMD_PROP_NONE, 0) != 0) return -1;
        if (commands_write_node(buf, sizeof(buf), &pos, 0x05, NULL, 0,
                                "kill", 0, CMD_PROP_NONE, 0) != 0) return -1;
    }

    /* 22: literal health -> [set|add|damage|dmg|heal|kill] */
    {
        const int children[] = {16, 17, 18, 19, 20, 21};
        if (commands_write_node(buf, sizeof(buf), &pos, 0x01, children, 6,
                                "health", 0, CMD_PROP_NONE, 0) != 0) return -1;
    }

    /* 23: literal kill */
    if (commands_write_node(buf, sizeof(buf), &pos, 0x05, NULL, 0,
                            "kill", 0, CMD_PROP_NONE, 0) != 0) return -1;

    /* 24-27: difficulty values */
    if (commands_write_node(buf, sizeof(buf), &pos, 0x05, NULL, 0,
                            "peaceful", 0, CMD_PROP_NONE, 0) != 0) return -1;
    if (commands_write_node(buf, sizeof(buf), &pos, 0x05, NULL, 0,
                            "easy", 0, CMD_PROP_NONE, 0) != 0) return -1;
    if (commands_write_node(buf, sizeof(buf), &pos, 0x05, NULL, 0,
                            "normal", 0, CMD_PROP_NONE, 0) != 0) return -1;
    if (commands_write_node(buf, sizeof(buf), &pos, 0x05, NULL, 0,
                            "hard", 0, CMD_PROP_NONE, 0) != 0) return -1;

    /* 28: difficulty get */
    if (commands_write_node(buf, sizeof(buf), &pos, 0x05, NULL, 0,
                            "get", 0, CMD_PROP_NONE, 0) != 0) return -1;

    /* 29: difficulty set -> [peaceful|easy|normal|hard] */
    {
        const int children[] = {24, 25, 26, 27};
        if (commands_write_node(buf, sizeof(buf), &pos, 0x01, children, 4,
                                "set", 0, CMD_PROP_NONE, 0) != 0) return -1;
    }

    /* 30: literal difficulty -> [peaceful|easy|normal|hard|get|set] */
    {
        const int children[] = {24, 25, 26, 27, 28, 29};
        if (commands_write_node(buf, sizeof(buf), &pos, 0x01, children, 6,
                                "difficulty", 0, CMD_PROP_NONE, 0) != 0) return -1;
    }

    /* 31: root -> [gamemode|tp|setblock|food|health|kill|difficulty] */
    {
        const int children[] = {5, 6, 9, 15, 22, 23, 30};
        if (commands_write_node(buf, sizeof(buf), &pos, 0x00, children, 7,
                                NULL, 0, CMD_PROP_NONE, 0) != 0) return -1;
    }

    if (w_varint(buf, sizeof(buf), &pos, 31) != 0) return -1; /* root index */
    return conn_write_packet(c, PKT_PLAY_COMMANDS, buf, pos, -1);
}

static int parse_gamemode(const char *mode) {
    if (!mode) return -1;
    char tmp[32];
    size_t n = strlen(mode);
    if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
    for (size_t i = 0; i < n; i++) tmp[i] = (char)tolower((unsigned char)mode[i]);
    tmp[n] = '\0';
    if (strcmp(tmp, "0") == 0 || strcmp(tmp, "s") == 0 || strcmp(tmp, "survival") == 0) return GAMEMODE_SURVIVAL;
    if (strcmp(tmp, "1") == 0 || strcmp(tmp, "c") == 0 || strcmp(tmp, "creative") == 0) return GAMEMODE_CREATIVE;
    if (strcmp(tmp, "2") == 0 || strcmp(tmp, "a") == 0 || strcmp(tmp, "adventure") == 0) return GAMEMODE_ADVENTURE;
    if (strcmp(tmp, "3") == 0 || strcmp(tmp, "sp") == 0 || strcmp(tmp, "spectator") == 0) return GAMEMODE_SPECTATOR;
    return -1;
}

static bool parse_double(const char *s, double *out) {
    if (!s || !*s) return false;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || *end != '\0') return false;
    *out = v;
    return true;
}

static bool parse_i32(const char *s, int32_t *out) {
    if (!s || !*s || !out) return false;
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return false;
    if (v < INT32_MIN || v > INT32_MAX) return false;
    *out = (int32_t)v;
    return true;
}

static bool parse_f32_text(const char *s, float *out) {
    char *end = NULL;
    float v = 0.0f;
    if (!s || !*s || !out) return false;
    errno = 0;
    v = strtof(s, &end);
    if (errno != 0 || end == s || *end != '\0' || !isfinite(v)) return false;
    *out = v;
    return true;
}

static mc_conn_t *resolve_player(mc_conn_t *self, const char *name, bool *needs_release) {
    if (needs_release) *needs_release = false;
    if (!self || !name) return NULL;
    if (self->server) {
        mc_conn_t *c = net_server_find_conn_by_name(self->server, name);
        if (c) {
            if (needs_release) *needs_release = true;
            return c;
        }
    }
    if (strcmp(self->username, name) == 0) return self;
    return NULL;
}

static bool debug_place_enabled(void) {
    static int init = 0;
    static bool enabled = false;
    if (!init) {
        const char *env = getenv("MC_DEBUG_PLACE");
        enabled = (env && *env && strcmp(env, "0") != 0);
        init = 1;
    }
    return enabled;
}

static bool debug_players_enabled(void) {
    static int init = 0;
    static bool enabled = false;
    if (!init) {
        const char *env = getenv("MC_DEBUG_PLAYERS");
        enabled = (env && *env && strcmp(env, "0") != 0);
        init = 1;
    }
    return enabled;
}

static bool debug_containers_enabled(void) {
    static int init = 0;
    static bool enabled = false;
    if (!init) {
        const char *env = getenv("MC_DEBUG_CONTAINERS");
        enabled = (env && *env && strcmp(env, "0") != 0);
        init = 1;
    }
    return enabled;
}

static bool debug_container_pos_match(int32_t x, int32_t y, int32_t z) {
    static int init = 0;
    static bool enabled = false;
    static int32_t target_x = 0;
    static int32_t target_y = 0;
    static int32_t target_z = 0;
    if (!init) {
        const char *env = getenv("MC_DEBUG_CONTAINER_POS");
        if (env && *env) {
            int32_t parsed_x = 0, parsed_y = 0, parsed_z = 0;
            if (sscanf(env, "%d,%d,%d", &parsed_x, &parsed_y, &parsed_z) == 3) {
                enabled = true;
                target_x = parsed_x;
                target_y = parsed_y;
                target_z = parsed_z;
            }
        }
        init = 1;
    }
    if (!debug_containers_enabled()) return false;
    if (!enabled) return true;
    return target_x == x && target_y == y && target_z == z;
}

static uint64_t fnv1a64_bytes(const uint8_t *data, size_t len, uint64_t seed) {
    uint64_t h = seed;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

void proto_fill_offline_uuid(const char *username, uint8_t out[16]) {
    static const char prefix[] = "OfflinePlayer:";
    uint64_t a = fnv1a64_bytes((const uint8_t *)prefix, sizeof(prefix) - 1, 1469598103934665603ULL);
    uint64_t b = fnv1a64_bytes((const uint8_t *)prefix, sizeof(prefix) - 1, 1099511628211ULL);
    if (username) {
        a = fnv1a64_bytes((const uint8_t *)username, strlen(username), a);
        b = fnv1a64_bytes((const uint8_t *)username, strlen(username), b);
    }
    for (int i = 0; i < 8; i++) {
        out[i] = (uint8_t)((a >> (56 - i * 8)) & 0xFF);
        out[8 + i] = (uint8_t)((b >> (56 - i * 8)) & 0xFF);
    }
    out[6] = (uint8_t)((out[6] & 0x0F) | 0x30);
    out[8] = (uint8_t)((out[8] & 0x3F) | 0x80);
}

static mc_inventory_t *conn_inventory(mc_conn_t *c) {
    return (c && c->player) ? &c->player->inventory : NULL;
}

static const mc_inventory_t *conn_inventory_const(const mc_conn_t *c) {
    return (c && c->player) ? &c->player->inventory : NULL;
}

static int resolve_player_entity_type_id(void) {
    static int cached = -1;
    if (cached >= 0) return cached;

    const char *env = getenv("MC_PLAYER_ENTITY_TYPE_ID");
    int32_t from_env = -1;
    if (env && parse_i32(env, &from_env) && from_env >= 0) {
        cached = from_env;
    } else {
        cached = mc_minecraft_entity_type_id("minecraft:player");
    }
    if (cached < 0) log_error("failed to resolve entity type id for minecraft:player");
    if (debug_players_enabled()) {
        const char *entity_name = mc_minecraft_entity_type_name(cached);
        log_info("players debug: resolved player entity type id=%d name=%s%s", cached, entity_name ? entity_name : "(unknown)",
                 (env && *env) ? " (env override)" : "");
    }
    return cached;
}

static mc_slot_t *player_window_slot(mc_player_data_t *player, int window_slot, int container_slot_count) {
    if (!player || container_slot_count < 0) return NULL;
    int main_first = container_slot_count;
    int hotbar_first = main_first + 27;
    if (window_slot >= main_first && window_slot < hotbar_first) return &player->inventory.slots[9 + (window_slot - main_first)];
    if (window_slot >= hotbar_first && window_slot < hotbar_first + 9) return &player->inventory.slots[36 + (window_slot - hotbar_first)];
    return NULL;
}

static const mc_slot_t *player_window_slot_const(const mc_player_data_t *player, int window_slot, int container_slot_count) {
    if (!player || container_slot_count < 0) return NULL;
    int main_first = container_slot_count;
    int hotbar_first = main_first + 27;
    if (window_slot >= main_first && window_slot < hotbar_first) return &player->inventory.slots[9 + (window_slot - main_first)];
    if (window_slot >= hotbar_first && window_slot < hotbar_first + 9) return &player->inventory.slots[36 + (window_slot - hotbar_first)];
    return NULL;
}

static mc_slot_t *active_container_slot(mc_conn_t *c, int window_slot) {
    if (!c || !c->active_window.open || !c->active_window.container) return NULL;
    if (window_slot < 0 || window_slot >= c->active_window.slot_count) return NULL;
    return &c->active_window.container->slots[window_slot];
}

static mc_nbt_tag_t *nbt_new_tag(mc_nbt_type_t type, const char *name) {
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

static mc_nbt_tag_t *nbt_new_string_tag(const char *name, const char *value) {
    mc_nbt_tag_t *tag = nbt_new_tag(MC_NBT_TAG_STRING, name);
    if (!tag) return NULL;
    tag->payload.string_val = strdup(value ? value : "");
    if (!tag->payload.string_val) {
        mc_nbt_free(tag);
        return NULL;
    }
    return tag;
}

static int make_translated_title(const char *translate_key, uint8_t **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    mc_nbt_tag_t *root = nbt_new_tag(MC_NBT_TAG_COMPOUND, NULL);
    if (!root) return -1;
    root->payload.compound.length = 1;
    root->payload.compound.children = (mc_nbt_tag_t **)calloc(1, sizeof(*root->payload.compound.children));
    if (!root->payload.compound.children) {
        mc_nbt_free(root);
        return -1;
    }
    root->payload.compound.children[0] = nbt_new_string_tag("translate", translate_key);
    if (!root->payload.compound.children[0]) {
        mc_nbt_free(root);
        return -1;
    }
    int rc = mc_nbt_write_unnamed_root(root, out, out_len);
    mc_nbt_free(root);
    return rc;
}

static void close_active_window_local(mc_conn_t *c) {
    if (!c || !c->active_window.open) return;
    if (c->active_window.container) {
        mc_container_instance_clear(c->active_window.container);
        free(c->active_window.container);
    }
    memset(&c->active_window, 0, sizeof(c->active_window));
}

int32_t proto_play_item_to_state(const mc_world_ids_t *ids, int32_t item_id) {
    if (ids) {
        int32_t water_bucket = mc_minecraft_item_id("minecraft:water_bucket");
        if (item_id == water_bucket) return ids->water_level[0];
        int32_t lava_bucket = mc_minecraft_item_id("minecraft:lava_bucket");
        if (item_id == lava_bucket) return ids->lava_level[0];
    }
    return mc_item_default_place_state(item_id);
}

int32_t proto_play_slot_to_state(const mc_world_ids_t *ids, const mc_slot_t *slot) {
    if (!slot || !slot->present || slot->count <= 0) return -1;
    return proto_play_item_to_state(ids, slot->item_id);
}

int32_t proto_play_resolve_placement_state(const mc_world_ids_t *ids, const mc_slot_t *slot, int32_t face, float yaw, float pitch) {
    (void)ids;
    (void)pitch;
    return mc_resolve_placement_state(slot, yaw, face);
}

static const char *axis_from_clicked_face(int32_t face) {
    if (face == 0 || face == 1) return "y";
    if (face == 2 || face == 3) return "z";
    if (face == 4 || face == 5) return "x";
    return NULL;
}

static int floor_i32_from_f32(float v) {
    int i = (int)v;
    if ((float)i > v) i--;
    return i;
}

static const char *opposite_horizontal_facing_from_yaw(float yaw) {
    int rot = floor_i32_from_f32((yaw * 4.0f / 360.0f) + 0.5f) & 3;
    switch (rot) {
        case 0: return "north";
        case 1: return "east";
        case 2: return "south";
        case 3: return "west";
        default: return "north";
    }
}

static int rewrite_state_property(const char *src, const char *prop, const char *value, char *dst, size_t dst_cap) {
    if (!src || !prop || !value || !dst || dst_cap == 0) return -1;
    const char *match = strstr(src, prop);
    if (!match) return -1;
    const char *value_start = match + strlen(prop);
    const char *value_end = value_start;
    while (*value_end && *value_end != ',' && *value_end != ']') value_end++;

    size_t prefix_len = (size_t)(value_start - src);
    size_t suffix_len = strlen(value_end);
    size_t value_len = strlen(value);
    if (prefix_len + value_len + suffix_len + 1 > dst_cap) return -1;

    memcpy(dst, src, prefix_len);
    memcpy(dst + prefix_len, value, value_len);
    memcpy(dst + prefix_len + value_len, value_end, suffix_len + 1);
    return 0;
}

static int32_t mc_resolve_placement_state(const mc_slot_t *slot, float player_yaw, int32_t clicked_face) {
    if (!slot || !slot->present || slot->count <= 0) return -1;

    int32_t mapped_state_id = mc_item_default_place_state(slot->item_id);
    if (mapped_state_id < 0) return -1;

    const char *mapped_key = mc_block_state_key(mapped_state_id);
    if (!mapped_key) return mapped_state_id;

    char rewritten[256];
    if (strstr(mapped_key, "axis=") != NULL) {
        const char *axis = axis_from_clicked_face(clicked_face);
        if (axis && rewrite_state_property(mapped_key, "axis=", axis, rewritten, sizeof(rewritten)) == 0) {
            return mc_block_state_id(rewritten, mapped_state_id);
        }
    }

    if (strstr(mapped_key, "facing=") != NULL) {
        const char *facing = opposite_horizontal_facing_from_yaw(player_yaw);
        if (facing && rewrite_state_property(mapped_key, "facing=", facing, rewritten, sizeof(rewritten)) == 0) {
            return mc_block_state_id(rewritten, mapped_state_id);
        }
    }

    return mapped_state_id;
}

static int write_slot_item(mc_buf_t *b, const mc_slot_t *slot) {
    uint8_t raw[1024];
    size_t pos = 0;
    if (mc_slot_write_net(raw, sizeof(raw), &pos, slot) != 0) return -1;
    return buf_write(b, raw, pos);
}

static int send_set_slot_packet(mc_conn_t *c, int32_t window_id, int32_t state_id, int16_t slot, const mc_slot_t *item) {
    mc_buf_t payload;
    if (buf_init(&payload, 64) != 0) return -1;
    int rc = 0;
    if (buf_w_varint(&payload, window_id) != 0 || buf_w_varint(&payload, state_id) != 0 || buf_w_u16_be(&payload, (uint16_t)slot) != 0 ||
        write_slot_item(&payload, item) != 0) {
        rc = -1;
    } else {
        rc = conn_write_packet(c, PKT_PLAY_SET_SLOT, payload.data, payload.len, -1);
    }
    buf_free(&payload);
    return rc;
}

static int send_held_item_slot(mc_conn_t *c) {
    const mc_inventory_t *inv = conn_inventory_const(c);
    if (!c || !inv) return -1;
    uint8_t buf[8];
    size_t pos = 0;
    if (w_byte(buf, sizeof(buf), &pos, (int8_t)inv->selected_hotbar_slot) != 0) return -1;
    return conn_write_packet(c, PKT_PLAY_HELD_ITEM_SLOT, buf, pos, -1);
}

static int sync_inventory_after_crafting_close(mc_conn_t *c) {
    if (!c || !c->player) return -1;
    if (update_player_crafting_result(c) != 0) {
        log_error("crafting close sync failed: player result update");
        return -1;
    }
    if (send_window_items(c) != 0) {
        log_error("crafting close sync failed: inventory content packet");
        return -1;
    }
    if (send_held_item_slot(c) != 0) {
        log_error("crafting close sync failed: held item slot packet");
        return -1;
    }
    return 0;
}

static int send_window_items(mc_conn_t *c) {
    const mc_inventory_t *inv = conn_inventory_const(c);
    if (!c || !inv) return -1;
    mc_buf_t payload;
    if (buf_init(&payload, 2048) != 0) return -1;
    int rc = 0;
    if (buf_w_varint(&payload, 0) != 0 || buf_w_varint(&payload, inv->state_id) != 0 || buf_w_varint(&payload, MC_PLAYER_SLOT_COUNT) != 0) {
        rc = -1;
        goto done;
    }
    for (int i = 0; i < MC_PLAYER_SLOT_COUNT; i++) {
        if (write_slot_item(&payload, &inv->slots[i]) != 0) {
            rc = -1;
            goto done;
        }
    }
    if (write_slot_item(&payload, &inv->cursor_slot) != 0) {
        rc = -1;
        goto done;
    }
    rc = conn_write_packet(c, PKT_PLAY_WINDOW_ITEMS, payload.data, payload.len, -1);
done:
    buf_free(&payload);
    return rc;
}

static int save_active_window(mc_conn_t *c) {
    if (!c || !c->active_window.open || !c->active_window.container) return 0;
    mc_container_instance_t *container = c->active_window.container;
    if (container->kind == MC_CONTAINER_KIND_CRAFTING_TABLE) return 0;
    if (container->kind == MC_CONTAINER_KIND_ENDER_CHEST) {
        return save_player_data(c);
    }
    if (!container->dirty) return 0;
    mc_world_t *world = get_world(c);
    if (!world) return 0;
    int32_t state_id = -1;
    if (mc_world_get_block(world, container->x, container->y, container->z, &state_id) != 0) return -1;
    if (mc_world_mark_chunk_dirty_at(world, container->x, container->z) != 0) return -1;
    mc_block_entity_type_t type = container_entity_type_for_state(state_id);
    mc_block_entity_t entity;
    if (type == MC_BLOCK_ENTITY_NONE) return 0;
    if (block_entity_from_container_instance(&entity, type, container) != 0 ||
        mc_world_put_block_entity(world, container->x, container->y, container->z, &entity) != 0 ||
        mc_world_flush_block(world, container->x, container->y, container->z) != 0) {
        log_error("container save failed kind=%d pos=(%d,%d,%d)", (int)container->kind, container->x, container->y, container->z);
        return -1;
    }
    container->dirty = false;
    return 0;
}

static int send_open_window(mc_conn_t *c, int32_t window_id, int32_t window_type, const char *translate_key) {
    uint8_t *title = NULL;
    size_t title_len = 0;
    if (make_translated_title(translate_key, &title, &title_len) != 0) return -1;
    mc_buf_t payload;
    if (buf_init(&payload, 64 + title_len) != 0) {
        free(title);
        return -1;
    }
    int rc = 0;
    if (buf_w_varint(&payload, window_id) != 0 || buf_w_varint(&payload, window_type) != 0 ||
        buf_write(&payload, title, title_len) != 0) {
        rc = -1;
    } else {
        rc = conn_write_packet(c, PKT_PLAY_OPEN_WINDOW, payload.data, payload.len, -1);
    }
    free(title);
    buf_free(&payload);
    return rc;
}

static int send_close_window(mc_conn_t *c, int32_t window_id) {
    uint8_t payload[8];
    size_t pos = 0;
    if (w_varint(payload, sizeof(payload), &pos, window_id) != 0) return -1;
    return conn_write_packet(c, PKT_PLAY_CLOSE_WINDOW, payload, pos, -1);
}

static int send_container_window_items(mc_conn_t *c) {
    if (!c || !c->player || !c->active_window.open || !c->active_window.container) return -1;
    mc_container_instance_t *container = c->active_window.container;
    mc_buf_t payload;
    if (buf_init(&payload, 4096) != 0) return -1;
    int total_slots = container->slot_count + 36;
    int rc = 0;
    if (buf_w_varint(&payload, c->active_window.window_id) != 0 || buf_w_varint(&payload, container->state_id) != 0 ||
        buf_w_varint(&payload, total_slots) != 0) {
        rc = -1;
        goto done;
    }
    for (int i = 0; i < container->slot_count; i++) {
        if (write_slot_item(&payload, &container->slots[i]) != 0) {
            rc = -1;
            goto done;
        }
    }
    for (int i = container->slot_count; i < container->slot_count + 36; i++) {
        const mc_slot_t *slot = player_window_slot_const(c->player, i, container->slot_count);
        if (!slot || write_slot_item(&payload, slot) != 0) {
            rc = -1;
            goto done;
        }
    }
    if (write_slot_item(&payload, &c->player->inventory.cursor_slot) != 0) {
        rc = -1;
        goto done;
    }
    rc = conn_write_packet(c, PKT_PLAY_WINDOW_ITEMS, payload.data, payload.len, -1);
done:
    buf_free(&payload);
    return rc;
}

static int furnace_data_i16(int32_t value) {
    if (value < 0) return 0;
    if (value > INT16_MAX) return INT16_MAX;
    return value;
}

static int send_container_data_packet(mc_conn_t *c, int32_t window_id, int16_t property, int16_t value) {
    if (!c) return -1;
    mc_buf_t payload;
    if (buf_init(&payload, 16) != 0) return -1;
    int rc = 0;
    if (buf_w_u8(&payload, (uint8_t)window_id) != 0 ||
        buf_w_u16_be(&payload, (uint16_t)property) != 0 ||
        buf_w_u16_be(&payload, (uint16_t)value) != 0) {
        rc = -1;
    } else {
        rc = conn_write_packet(c, PKT_PLAY_CONTAINER_SET_DATA, payload.data, payload.len, -1);
    }
    buf_free(&payload);
    return rc;
}

static int send_furnace_container_data(mc_conn_t *c, const mc_container_instance_t *container) {
    if (!c || !container || !mc_furnace_container_kind_is_machine(container->kind)) return 0;
    int32_t window_id = c->active_window.window_id;
    if (send_container_data_packet(c, window_id, 0, (int16_t)furnace_data_i16(container->furnace_burn_time)) != 0) return -1;
    if (send_container_data_packet(c, window_id, 1, (int16_t)furnace_data_i16(container->furnace_burn_duration)) != 0) return -1;
    if (send_container_data_packet(c, window_id, 2, (int16_t)furnace_data_i16(container->furnace_cook_time)) != 0) return -1;
    if (send_container_data_packet(c, window_id, 3, (int16_t)furnace_data_i16(container->furnace_cook_duration)) != 0) return -1;
    return 0;
}

static int sync_active_container_window(mc_conn_t *c) {
    if (send_container_window_items(c) != 0) return -1;
    if (c && c->active_window.open && c->active_window.container &&
        mc_furnace_container_kind_is_machine(c->active_window.container->kind)) {
        return send_furnace_container_data(c, c->active_window.container);
    }
    return 0;
}

static int empty_optional_nbt(uint8_t **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    uint8_t *buf = (uint8_t *)malloc(2);
    if (!buf) return -1;
    buf[0] = 0x0A;
    buf[1] = 0x00;
    if (out) *out = buf;
    if (out_len) *out_len = 2;
    return 0;
}

static int container_block_entity_chunk_nbt(mc_world_t *world, int32_t state_id, int32_t x, int32_t y, int32_t z, uint8_t **out,
                                            size_t *out_len) {
    (void)world;
    (void)x;
    (void)y;
    (void)z;
    if (container_block_entity_type(state_id) < 0) return empty_optional_nbt(out, out_len);
    /* Vanilla 1.21.x serializes basic container block entities in chunk data with
     * an empty network Compound root, not metadata tags like id/x/y/z. */
    return empty_optional_nbt(out, out_len);
}

static int send_block_entity_data_packet(mc_conn_t *c, int32_t state_id, int32_t x, int32_t y, int32_t z) {
    if (!c) return -1;
    int32_t be_type = container_block_entity_type(state_id);
    if (be_type < 0) return 0;

    uint8_t *nbt = NULL;
    size_t nbt_len = 0;
    if (container_block_entity_chunk_nbt(get_world(c), state_id, x, y, z, &nbt, &nbt_len) != 0) return -1;

    mc_buf_t payload;
    if (buf_init(&payload, 64 + nbt_len) != 0) {
        free(nbt);
        return -1;
    }

    int rc = 0;
    uint8_t posbuf[16];
    size_t p = 0;
    if (w_position(posbuf, sizeof(posbuf), &p, x, y, z) != 0 || buf_write(&payload, posbuf, p) != 0 ||
        buf_w_varint(&payload, be_type) != 0 || buf_write(&payload, nbt, nbt_len) != 0) {
        rc = -1;
    } else {
        rc = conn_write_packet(c, PKT_PLAY_TILE_ENTITY_DATA, payload.data, payload.len, -1);
    }
    free(nbt);
    buf_free(&payload);
    return rc;
}

static int block_action_block_id_for_state(int32_t state_id) {
    if (state_id >= 0 && (size_t)state_id < GLOBAL_BLOCK_STATES_COUNT) {
        return (int32_t)GLOBAL_BLOCK_STATES[state_id].block_index;
    }
    return state_id;
}

static int send_block_event_packet(mc_conn_t *c, int32_t x, int32_t y, int32_t z, uint8_t action, uint8_t param, int32_t block_id) {
    if (!c) return -1;
    uint8_t buf[32];
    size_t pos = 0;
    if (w_position(buf, sizeof(buf), &pos, x, y, z) != 0 || w_ubyte(buf, sizeof(buf), &pos, action) != 0 ||
        w_ubyte(buf, sizeof(buf), &pos, param) != 0 || w_varint(buf, sizeof(buf), &pos, block_id) != 0) {
        return -1;
    }
    return conn_write_packet(c, PKT_PLAY_BLOCK_EVENT, buf, pos, -1);
}

static int send_block_update_packet(mc_conn_t *c, int32_t x, int32_t y, int32_t z, int32_t state_id) {
    if (!c) return -1;
    mc_buf_t payload;
    if (buf_init(&payload, 32) != 0) return -1;
    uint8_t posbuf[16];
    size_t p = 0;
    int rc = 0;
    if (w_position(posbuf, sizeof(posbuf), &p, x, y, z) != 0 || buf_write(&payload, posbuf, p) != 0 ||
        buf_w_varint(&payload, state_id) != 0) {
        rc = -1;
    } else {
        rc = conn_write_packet(c, PKT_PLAY_BLOCK_UPDATE, payload.data, payload.len, -1);
    }
    buf_free(&payload);
    return rc;
}

static int send_block_changed_ack_packet(mc_conn_t *c, int32_t sequence) {
    if (!c) return -1;
    uint8_t buf[8];
    size_t pos = 0;
    if (w_varint(buf, sizeof(buf), &pos, sequence) != 0) return -1;
    return conn_write_packet(c, PKT_PLAY_BLOCK_CHANGED_ACK, buf, pos, -1);
}

static int resend_authoritative_chunk_at(mc_conn_t *c, mc_world_t *world, int32_t x, int32_t z) {
    if (!c || !world) return -1;
    int32_t cx = (x >= 0) ? (x / CHUNK_XZ) : ((x - (CHUNK_XZ - 1)) / CHUNK_XZ);
    int32_t cz = (z >= 0) ? (z / CHUNK_XZ) : ((z - (CHUNK_XZ - 1)) / CHUNK_XZ);
    mc_chunk_t *chunk = mc_world_get_chunk(world, cx, cz, UINT32_MAX);
    if (!chunk) return -1;
    if (send_chunk_ready(c, world, cx, cz, chunk) != 0) return -1;
    if (debug_container_pos_match(x, MC_WORLD_MIN_Y, z) || debug_containers_enabled()) {
        log_info("containers debug: authoritative chunk resend chunk=(%d,%d)", cx, cz);
    }
    return 0;
}

static void close_active_window(mc_conn_t *c, bool notify_client) {
    if (!c || !c->active_window.open) return;
    bool sync_player_inventory_after_close = false;
    bool save_player_after_close = false;
    if (c->active_window.container) {
        if (c->active_window.container->kind == MC_CONTAINER_KIND_CRAFTING_TABLE) {
            int return_rc = return_container_crafting_grid(c, c->active_window.container);
            if (return_rc < 0) {
                log_error("crafting table close failed: could not return grid window_id=%d", c->active_window.window_id);
                conn_close(c);
                return;
            }
            sync_player_inventory_after_close = true;
            save_player_after_close = true;
        }
        mc_world_t *world = get_world(c);
        int32_t state_id = -1;
        int32_t x = c->active_window.container->x;
        int32_t y = c->active_window.container->y;
        int32_t z = c->active_window.container->z;
        if (world && mc_world_get_block(world, x, y, z, &state_id) == 0 && is_shulker_box_state(state_id)) {
            (void)send_block_event_packet(c, x, y, z, 1, 0, block_action_block_id_for_state(state_id));
        }
    }
    if (save_active_window(c) != 0) {
        conn_close(c);
        return;
    }
    int32_t window_id = c->active_window.window_id;
    close_active_window_local(c);
    if (notify_client) (void)send_close_window(c, window_id);
    if (sync_player_inventory_after_close && sync_inventory_after_crafting_close(c) != 0) {
        log_error("crafting close sync failed after closing window_id=%d", window_id);
    }
    if (save_player_after_close && save_player_data(c) != 0) conn_close(c);
}

static int open_container_window_typed(mc_conn_t *c, mc_container_instance_t *container, int32_t window_type, const char *title_key) {
    if (!c || !container) return -1;
    close_active_window(c, false);
    c->active_window.open = true;
    c->active_window.window_id = ++c->next_window_id;
    if (c->next_window_id <= 0) c->next_window_id = 1;
    c->active_window.window_type = window_type;
    c->active_window.slot_count = container->slot_count;
    c->active_window.container = container;
    if (send_open_window(c, c->active_window.window_id, c->active_window.window_type, title_key) != 0) return -1;
    return sync_active_container_window(c);
}

static int sync_inventory_full(mc_conn_t *c) {
    if (update_player_crafting_result(c) != 0) return -1;
    if (send_window_items(c) != 0) return -1;
    return send_held_item_slot(c);
}

static int sync_inventory_slot(mc_conn_t *c, int16_t slot) {
    mc_inventory_t *inv = conn_inventory(c);
    if (!c || !inv) return -1;
    if (slot < 0 || slot >= MC_PLAYER_SLOT_COUNT) return -1;
    return send_set_slot_packet(c, 0, inv->state_id, slot, &inv->slots[slot]);
}

static int update_player_crafting_result(mc_conn_t *c) {
    if (!c || !c->player) return 0;
    return mc_crafting_update_result(&c->player->inventory.slots[PLAYER_CRAFTING_RESULT_SLOT],
                                     &c->player->inventory.slots[PLAYER_CRAFTING_GRID_SLOT],
                                     PLAYER_CRAFTING_GRID_WIDTH, PLAYER_CRAFTING_GRID_HEIGHT);
}

static int update_container_crafting_result(mc_container_instance_t *container) {
    if (!container || container->kind != MC_CONTAINER_KIND_CRAFTING_TABLE) return 0;
    return mc_crafting_update_result(&container->slots[CRAFTING_TABLE_RESULT_SLOT],
                                     &container->slots[CRAFTING_TABLE_GRID_SLOT],
                                     CRAFTING_TABLE_GRID_WIDTH, CRAFTING_TABLE_GRID_HEIGHT);
}

static int return_crafting_grid_to_inventory(mc_conn_t *c, mc_slot_t *grid, int grid_slots) {
    if (!c || !c->player || !grid || grid_slots <= 0) return 0;
    int rc = 0;
    for (int i = 0; i < grid_slots; i++) {
        mc_slot_t *slot = &grid[i];
        if (!slot->present || slot->count <= 0) continue;
        int absorbed = mc_inventory_try_absorb_slot(&c->player->inventory, slot);
        if (absorbed < 0) return -1;
        if (slot->present && slot->count > 0) {
            int drop_rc = drop_player_slot(c, slot, false);
            if (drop_rc < 0) return -1;
            if (drop_rc > 0) rc = 1;
        } else if (absorbed > 0) {
            rc = 1;
        }
    }
    if (rc > 0) c->player->inventory.state_id++;
    return rc;
}

static int return_player_crafting_grid(mc_conn_t *c) {
    if (!c || !c->player) return 0;
    mc_crafting_clear_result(&c->player->inventory.slots[PLAYER_CRAFTING_RESULT_SLOT]);
    int rc = return_crafting_grid_to_inventory(c, &c->player->inventory.slots[PLAYER_CRAFTING_GRID_SLOT],
                                               PLAYER_CRAFTING_GRID_WIDTH * PLAYER_CRAFTING_GRID_HEIGHT);
    if (rc < 0) return -1;
    c->player->inventory.state_id++;
    return rc;
}

static int return_container_crafting_grid(mc_conn_t *c, mc_container_instance_t *container) {
    if (!c || !container || container->kind != MC_CONTAINER_KIND_CRAFTING_TABLE) return 0;
    mc_crafting_clear_result(&container->slots[CRAFTING_TABLE_RESULT_SLOT]);
    int rc = return_crafting_grid_to_inventory(c, &container->slots[CRAFTING_TABLE_GRID_SLOT],
                                               CRAFTING_TABLE_GRID_WIDTH * CRAFTING_TABLE_GRID_HEIGHT);
    if (rc < 0) return -1;
    container->state_id++;
    return rc;
}

static int take_player_crafting_result(mc_conn_t *c) {
    if (!c || !c->player) return -1;
    mc_inventory_t *inv = &c->player->inventory;
    int rc = mc_crafting_take_result(&inv->slots[PLAYER_CRAFTING_RESULT_SLOT],
                                     &inv->slots[PLAYER_CRAFTING_GRID_SLOT],
                                     PLAYER_CRAFTING_GRID_WIDTH, PLAYER_CRAFTING_GRID_HEIGHT,
                                     &inv->cursor_slot);
    if (rc <= 0) return rc;
    inv->state_id++;
    if (sync_inventory_full(c) != 0) return -1;
    return save_player_data(c);
}

static int take_container_crafting_result(mc_conn_t *c, mc_container_instance_t *container) {
    if (!c || !c->player || !container || container->kind != MC_CONTAINER_KIND_CRAFTING_TABLE) return -1;
    int rc = mc_crafting_take_result(&container->slots[CRAFTING_TABLE_RESULT_SLOT],
                                     &container->slots[CRAFTING_TABLE_GRID_SLOT],
                                     CRAFTING_TABLE_GRID_WIDTH, CRAFTING_TABLE_GRID_HEIGHT,
                                     &c->player->inventory.cursor_slot);
    if (rc <= 0) return rc;
    container->state_id++;
    c->player->inventory.state_id++;
    if (sync_active_container_window(c) != 0) return -1;
    return save_player_data(c);
}

static int quick_move_player_crafting_result(mc_conn_t *c) {
    if (!c || !c->player) return -1;
    mc_inventory_t *inv = &c->player->inventory;
    int rc = mc_crafting_quick_move_result(&inv->slots[PLAYER_CRAFTING_RESULT_SLOT],
                                           &inv->slots[PLAYER_CRAFTING_GRID_SLOT],
                                           PLAYER_CRAFTING_GRID_WIDTH, PLAYER_CRAFTING_GRID_HEIGHT,
                                           inv);
    if (rc <= 0) return rc;
    if (sync_inventory_full(c) != 0) return -1;
    return save_player_data(c);
}

static int quick_move_container_crafting_result(mc_conn_t *c, mc_container_instance_t *container) {
    if (!c || !c->player || !container || container->kind != MC_CONTAINER_KIND_CRAFTING_TABLE) return -1;
    int rc = mc_crafting_quick_move_result(&container->slots[CRAFTING_TABLE_RESULT_SLOT],
                                           &container->slots[CRAFTING_TABLE_GRID_SLOT],
                                           CRAFTING_TABLE_GRID_WIDTH, CRAFTING_TABLE_GRID_HEIGHT,
                                           &c->player->inventory);
    if (rc <= 0) return rc;
    container->state_id++;
    if (sync_active_container_window(c) != 0) return -1;
    return save_player_data(c);
}

int proto_play_try_pickup_ground_slot(mc_conn_t *c, mc_slot_t *ground_slot) {
    if (!c || !ground_slot) return -1;
    if (c->closing || c->state != MC_STATE_PLAY || !c->player || !c->play_ready || c->dead) return 0;
    if (c->active_window.open) return 0;

    int absorbed = mc_inventory_try_absorb_slot(&c->player->inventory, ground_slot);
    if (absorbed <= 0) return absorbed;
    if (sync_inventory_full(c) != 0) return -1;
    if (save_player_data(c) != 0) return -1;
    return absorbed;
}

static void world_spawn_values(double *x, double *y, double *z, float *yaw, float *pitch) {
    if (x) *x = WORLD_SPAWN_X;
    if (y) *y = WORLD_SPAWN_Y;
    if (z) *z = WORLD_SPAWN_Z;
    if (yaw) *yaw = WORLD_SPAWN_YAW;
    if (pitch) *pitch = WORLD_SPAWN_PITCH;
}

static void reset_fall_tracking(mc_conn_t *c) {
    if (!c) return;
    c->on_ground = false;
    c->fall_tracking = false;
    c->fall_start_y = c->y;
    c->next_void_damage_ms = 0;
}

static bool player_can_use_hunger_system(const mc_conn_t *c) {
    if (!c || !c->player || c->dead || c->closing || c->state != MC_STATE_PLAY) return false;
    return c->gamemode == GAMEMODE_SURVIVAL || c->gamemode == GAMEMODE_ADVENTURE;
}

static bool player_can_take_damage(const mc_conn_t *c) {
    if (!c || !c->player || c->dead || c->closing || c->state != MC_STATE_PLAY) return false;
    return c->gamemode != GAMEMODE_CREATIVE && c->gamemode != GAMEMODE_SPECTATOR;
}

static int player_sync_food_state(mc_conn_t *c, bool persist) {
    if (!c || !c->player) return -1;
    c->food = clamp_food_level(c->food);
    c->food_saturation = clamp_food_saturation(c->food, c->food_saturation);
    c->food_exhaustion = clamp_food_exhaustion(c->food_exhaustion);
    c->player->food_level = c->food;
    c->player->food_saturation = c->food_saturation;
    c->player->food_exhaustion = c->food_exhaustion;
    if (send_set_health_packet(c) != 0) return -1;
    if (persist && save_player_data(c) != 0) return -1;
    return 0;
}

static void player_reset_food_debug_runtime(mc_conn_t *c) {
    if (!c || !c->player) return;
    cancel_item_use(c);
    c->food = clamp_food_level(c->food);
    c->food_saturation = clamp_food_saturation(c->food, c->food_saturation);
    c->food_exhaustion = 0.0f;
    c->next_natural_regen_ms = 0;
    c->next_starvation_damage_ms = 0;
    c->player->food_level = c->food;
    c->player->food_saturation = c->food_saturation;
    c->player->food_exhaustion = c->food_exhaustion;
}

static int player_apply_exhaustion_thresholds(mc_conn_t *c, bool persist) {
    bool changed = false;

    if (!c || !c->player) return -1;
    c->food = clamp_food_level(c->food);
    c->food_saturation = clamp_food_saturation(c->food, c->food_saturation);
    c->food_exhaustion = clamp_food_exhaustion(c->food_exhaustion);

    while (c->food_exhaustion >= PLAYER_FOOD_EXHAUSTION_STEP) {
        c->food_exhaustion -= PLAYER_FOOD_EXHAUSTION_STEP;
        if (c->food_saturation > 0.0f) {
            c->food_saturation -= 1.0f;
            if (c->food_saturation < 0.0f) c->food_saturation = 0.0f;
            changed = true;
            continue;
        }
        if (c->food > 0) {
            c->food -= 1;
            changed = true;
        }
    }

    if (!changed) {
        c->player->food_level = c->food;
        c->player->food_saturation = c->food_saturation;
        c->player->food_exhaustion = c->food_exhaustion;
        if (persist && save_player_data(c) != 0) return -1;
        return 0;
    }
    return player_sync_food_state(c, persist);
}

static int player_add_exhaustion(mc_conn_t *c, float amount, bool persist) {
    if (!player_can_use_hunger_system(c)) return 0;
    if (!(amount > 0.0f) || !isfinite(amount)) return 0;
    if (current_difficulty(c) == MC_DIFFICULTY_PEACEFUL) {
        c->food_exhaustion = 0.0f;
        if (c->player) c->player->food_exhaustion = 0.0f;
        return persist ? save_player_data(c) : 0;
    }
    c->food_exhaustion = clamp_food_exhaustion(c->food_exhaustion + amount);
    c->player->food_exhaustion = c->food_exhaustion;
    return player_apply_exhaustion_thresholds(c, persist);
}

static int make_text_component(const char *text, uint8_t **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    mc_nbt_tag_t *root = nbt_new_tag(MC_NBT_TAG_COMPOUND, NULL);
    if (!root) return -1;
    root->payload.compound.length = 1;
    root->payload.compound.children = (mc_nbt_tag_t **)calloc(1, sizeof(*root->payload.compound.children));
    if (!root->payload.compound.children) {
        mc_nbt_free(root);
        return -1;
    }
    root->payload.compound.children[0] = nbt_new_string_tag("text", text ? text : "");
    if (!root->payload.compound.children[0]) {
        mc_nbt_free(root);
        return -1;
    }
    int rc = mc_nbt_write_unnamed_root(root, out, out_len);
    mc_nbt_free(root);
    return rc;
}

static int send_set_health_packet(mc_conn_t *c) {
    if (!c) return -1;
    uint8_t buf[32];
    size_t pos = 0;
    float health = c->health;
    int32_t food = clamp_food_level(c->food);
    float food_saturation = clamp_food_saturation(food, c->food_saturation);
    if (health < 0.0f) health = 0.0f;
    if (health > PLAYER_MAX_HEALTH) health = PLAYER_MAX_HEALTH;
    if (w_f32(buf, sizeof(buf), &pos, health) != 0) return -1;
    if (w_varint(buf, sizeof(buf), &pos, food) != 0) return -1;
    if (w_f32(buf, sizeof(buf), &pos, food_saturation) != 0) return -1;
    return conn_write_packet(c, PKT_PLAY_SET_HEALTH, buf, pos, -1);
}

static int send_combat_kill_packet(mc_conn_t *c, const char *message) {
    if (!c) return -1;
    uint8_t *msg = NULL;
    size_t msg_len = 0;
    if (make_text_component(message ? message : "You died", &msg, &msg_len) != 0) return -1;

    mc_buf_t payload;
    if (buf_init(&payload, 64 + msg_len) != 0) {
        free(msg);
        return -1;
    }

    int rc = 0;
    if (buf_w_varint(&payload, c->entity_id) != 0 || buf_write(&payload, msg, msg_len) != 0) {
        rc = -1;
    } else {
        rc = conn_write_packet(c, PKT_PLAY_PLAYER_COMBAT_KILL, payload.data, payload.len, -1);
    }
    buf_free(&payload);
    free(msg);
    return rc;
}

static bool state_key_is_prefix(int32_t state_id, const char *prefix) {
    const char *key = mc_block_state_key(state_id);
    return key && prefix && strncmp(key, prefix, strlen(prefix)) == 0;
}

static bool state_key_contains(int32_t state_id, const char *needle) {
    const char *key = mc_block_state_key(state_id);
    return key && needle && strstr(key, needle) != NULL;
}

static bool block_state_is_replaceable(const mc_world_ids_t *ids, int32_t state_id) {
    if (!ids || state_id < 0) return false;
    if (state_id == ids->air) return true;
    for (int i = 0; i < 16; i++) {
        if (state_id == ids->water_level[i]) return true;
    }
    const char *key = mc_block_state_key(state_id);
    if (!key) return false;
    return strcmp(key, "minecraft:cave_air") == 0 ||
           strcmp(key, "minecraft:void_air") == 0 ||
           strcmp(key, "minecraft:short_grass") == 0 ||
           strncmp(key, "minecraft:tall_grass[", 21) == 0 ||
           strcmp(key, "minecraft:fern") == 0 ||
           strncmp(key, "minecraft:large_fern[", 21) == 0 ||
           strcmp(key, "minecraft:seagrass") == 0 ||
           strncmp(key, "minecraft:tall_seagrass[", 24) == 0 ||
           strcmp(key, "minecraft:dead_bush") == 0 ||
           strncmp(key, "minecraft:snow[", 15) == 0 ||
           strncmp(key, "minecraft:vine[", 15) == 0 ||
           strncmp(key, "minecraft:fire[", 15) == 0;
}

static bool block_state_has_player_blocking_collision(const mc_world_ids_t *ids, int32_t state_id) {
    if (state_id < 0) return false;
    if (block_state_is_replaceable(ids, state_id)) return false;

    const char *key = mc_block_state_key(state_id);
    if (!key) return true;

    return !(state_key_is_prefix(state_id, "minecraft:torch[") ||
             state_key_is_prefix(state_id, "minecraft:wall_torch[") ||
             state_key_is_prefix(state_id, "minecraft:redstone_torch[") ||
             state_key_is_prefix(state_id, "minecraft:redstone_wall_torch[") ||
             state_key_is_prefix(state_id, "minecraft:ladder[") ||
             state_key_is_prefix(state_id, "minecraft:lever[") ||
             state_key_is_prefix(state_id, "minecraft:button[") ||
             state_key_is_prefix(state_id, "minecraft:stone_button[") ||
             state_key_is_prefix(state_id, "minecraft:oak_button[") ||
             state_key_is_prefix(state_id, "minecraft:birch_button[") ||
             state_key_is_prefix(state_id, "minecraft:spruce_button[") ||
             state_key_is_prefix(state_id, "minecraft:jungle_button[") ||
             state_key_is_prefix(state_id, "minecraft:acacia_button[") ||
             state_key_is_prefix(state_id, "minecraft:dark_oak_button[") ||
             state_key_is_prefix(state_id, "minecraft:mangrove_button[") ||
             state_key_is_prefix(state_id, "minecraft:cherry_button[") ||
             state_key_is_prefix(state_id, "minecraft:bamboo_button[") ||
             state_key_is_prefix(state_id, "minecraft:crimson_button[") ||
             state_key_is_prefix(state_id, "minecraft:warped_button["));
}

static bool aabb_intersects_strict(double a_min_x, double a_min_y, double a_min_z, double a_max_x, double a_max_y, double a_max_z,
                                   double b_min_x, double b_min_y, double b_min_z, double b_max_x, double b_max_y, double b_max_z) {
    return a_min_x < b_max_x - PLAYER_COLLISION_EPSILON &&
           a_max_x > b_min_x + PLAYER_COLLISION_EPSILON &&
           a_min_y < b_max_y - PLAYER_COLLISION_EPSILON &&
           a_max_y > b_min_y + PLAYER_COLLISION_EPSILON &&
           a_min_z < b_max_z - PLAYER_COLLISION_EPSILON &&
           a_max_z > b_min_z + PLAYER_COLLISION_EPSILON;
}

static bool placed_block_intersects_player(const mc_conn_t *c, const mc_world_ids_t *ids, int32_t block_x, int32_t block_y, int32_t block_z,
                                           int32_t state_id) {
    if (!c || !c->has_pos) return false;
    if (!block_state_has_player_blocking_collision(ids, state_id)) return false;

    double player_min_x = c->x - PLAYER_COLLISION_HALF_WIDTH;
    double player_max_x = c->x + PLAYER_COLLISION_HALF_WIDTH;
    double player_min_y = c->y;
    double player_max_y = c->y + PLAYER_COLLISION_HEIGHT;
    double player_min_z = c->z - PLAYER_COLLISION_HALF_WIDTH;
    double player_max_z = c->z + PLAYER_COLLISION_HALF_WIDTH;

    return aabb_intersects_strict(player_min_x, player_min_y, player_min_z,
                                  player_max_x, player_max_y, player_max_z,
                                  (double)block_x, (double)block_y, (double)block_z,
                                  (double)block_x + 1.0, (double)block_y + 1.0, (double)block_z + 1.0);
}

static bool is_chest_state(int32_t state_id) {
    return state_key_contains(state_id, "chest[") && !is_trapped_chest_state(state_id) && !is_ender_chest_state(state_id);
}

static bool is_trapped_chest_state(int32_t state_id) {
    return state_key_contains(state_id, "trapped_chest[");
}

static bool is_ender_chest_state(int32_t state_id) {
    return state_key_contains(state_id, "ender_chest[");
}

static bool is_barrel_state(int32_t state_id) {
    return state_key_contains(state_id, "barrel[");
}

static bool is_dropper_state(int32_t state_id) {
    return state_key_is_prefix(state_id, "minecraft:dropper[") ||
           state_key_is_prefix(state_id, "minecraft:dispenser[") ||
           state_key_is_prefix(state_id, "minecraft:hopper[");
}

static bool is_furnace_state(int32_t state_id) {
    return state_key_is_prefix(state_id, "minecraft:furnace[");
}

static bool is_smoker_state(int32_t state_id) {
    return state_key_is_prefix(state_id, "minecraft:smoker[");
}

static bool is_blast_furnace_state(int32_t state_id) {
    return state_key_is_prefix(state_id, "minecraft:blast_furnace[");
}

static bool is_furnace_like_state(int32_t state_id) {
    return is_furnace_state(state_id) || is_smoker_state(state_id) || is_blast_furnace_state(state_id);
}

static bool is_shulker_box_state(int32_t state_id) {
    return state_key_contains(state_id, "shulker_box[");
}

static bool is_crafting_table_state(int32_t state_id) {
    return state_key_is_prefix(state_id, "minecraft:crafting_table");
}

static bool is_world_container_state(int32_t state_id) {
    return is_chest_state(state_id) || is_trapped_chest_state(state_id) || is_barrel_state(state_id) ||
           is_dropper_state(state_id) || is_shulker_box_state(state_id) || is_furnace_like_state(state_id);
}

static mc_container_kind_t container_kind_for_state(int32_t state_id) {
    if (is_furnace_state(state_id)) return MC_CONTAINER_KIND_FURNACE;
    if (is_smoker_state(state_id)) return MC_CONTAINER_KIND_SMOKER;
    if (is_blast_furnace_state(state_id)) return MC_CONTAINER_KIND_BLAST_FURNACE;
    if (is_world_container_state(state_id)) return MC_CONTAINER_KIND_CHEST;
    if (is_ender_chest_state(state_id)) return MC_CONTAINER_KIND_ENDER_CHEST;
    if (is_crafting_table_state(state_id)) return MC_CONTAINER_KIND_CRAFTING_TABLE;
    return MC_CONTAINER_KIND_NONE;
}

static int32_t window_type_for_container_kind(mc_container_kind_t kind) {
    switch (kind) {
        case MC_CONTAINER_KIND_CRAFTING_TABLE: return MC_WINDOW_TYPE_CRAFTING;
        case MC_CONTAINER_KIND_FURNACE: return MC_WINDOW_TYPE_FURNACE;
        case MC_CONTAINER_KIND_SMOKER: return MC_WINDOW_TYPE_SMOKER;
        case MC_CONTAINER_KIND_BLAST_FURNACE: return MC_WINDOW_TYPE_BLAST_FURNACE;
        default: return MC_WINDOW_TYPE_GENERIC_9X3;
    }
}

static const char *container_title_for_state(int32_t state_id) {
    if (is_chest_state(state_id) || is_trapped_chest_state(state_id)) return "container.chest";
    if (is_ender_chest_state(state_id)) return "container.enderchest";
    if (is_barrel_state(state_id)) return "container.barrel";
    if (is_furnace_state(state_id)) return "container.furnace";
    if (is_smoker_state(state_id)) return "container.smoker";
    if (is_blast_furnace_state(state_id)) return "container.blast_furnace";
    if (is_dropper_state(state_id)) return "container.dropper";
    if (is_shulker_box_state(state_id)) return "container.shulkerBox";
    return "container.chest";
}

static mc_block_entity_type_t container_entity_type_for_state(int32_t state_id) {
    if (is_chest_state(state_id) || is_trapped_chest_state(state_id)) return MC_BLOCK_ENTITY_CHEST;
    if (is_ender_chest_state(state_id)) return MC_BLOCK_ENTITY_ENDER_CHEST;
    if (is_barrel_state(state_id)) return MC_BLOCK_ENTITY_BARREL;
    if (is_furnace_state(state_id)) return MC_BLOCK_ENTITY_FURNACE;
    if (is_smoker_state(state_id)) return MC_BLOCK_ENTITY_SMOKER;
    if (is_blast_furnace_state(state_id)) return MC_BLOCK_ENTITY_BLAST_FURNACE;
    if (is_dropper_state(state_id)) return MC_BLOCK_ENTITY_DROPPER;
    if (is_shulker_box_state(state_id)) return MC_BLOCK_ENTITY_SHULKER_BOX;
    return MC_BLOCK_ENTITY_NONE;
}

static void container_instance_from_block_entity(mc_container_instance_t *dst, mc_container_kind_t kind, int32_t x, int32_t y, int32_t z,
                                                 const mc_block_entity_t *entity) {
    mc_container_instance_init(dst, kind, x, y, z);
    if (!entity) return;
    uint32_t slot_count = entity->data.container.slot_count;
    if (slot_count > MC_CONTAINER_SLOT_COUNT) slot_count = MC_CONTAINER_SLOT_COUNT;
    if (slot_count > 0) dst->slot_count = (int32_t)slot_count;
    if (mc_furnace_container_kind_is_machine(kind)) {
        if (dst->slot_count <= 0 || dst->slot_count > MC_FURNACE_SLOT_COUNT) dst->slot_count = MC_FURNACE_SLOT_COUNT;
        if (slot_count > MC_FURNACE_SLOT_COUNT) slot_count = MC_FURNACE_SLOT_COUNT;
        dst->furnace_burn_time = entity->data.container.furnace_burn_time;
        dst->furnace_burn_duration = entity->data.container.furnace_burn_duration;
        dst->furnace_cook_time = entity->data.container.furnace_cook_time;
        dst->furnace_cook_duration = entity->data.container.furnace_cook_duration;
    }
    for (uint32_t i = 0; i < slot_count; i++) {
        (void)mc_slot_copy(&dst->slots[i], &entity->data.container.slots[i]);
    }
}

static int block_entity_from_container_instance(mc_block_entity_t *dst, mc_block_entity_type_t type, const mc_container_instance_t *src) {
    if (!dst || !src) return -1;
    memset(dst, 0, sizeof(*dst));
    dst->type = type;
    dst->data.container.slot_count = src->slot_count > 0 ? (uint32_t)src->slot_count : MC_CONTAINER_SLOT_COUNT;
    if (dst->data.container.slot_count > MC_CONTAINER_SLOT_COUNT) dst->data.container.slot_count = MC_CONTAINER_SLOT_COUNT;
    if (mc_furnace_container_kind_is_machine(src->kind) && dst->data.container.slot_count > MC_FURNACE_SLOT_COUNT) {
        dst->data.container.slot_count = MC_FURNACE_SLOT_COUNT;
    }
    dst->data.container.furnace_burn_time = src->furnace_burn_time;
    dst->data.container.furnace_burn_duration = src->furnace_burn_duration;
    dst->data.container.furnace_cook_time = src->furnace_cook_time;
    dst->data.container.furnace_cook_duration = src->furnace_cook_duration;
    for (uint32_t i = 0; i < dst->data.container.slot_count; i++) {
        if (mc_slot_copy(&dst->data.container.slots[i], &src->slots[i]) != 0) {
            for (uint32_t j = 0; j < i; j++) mc_slot_clear(&dst->data.container.slots[j]);
            memset(dst, 0, sizeof(*dst));
            return -1;
        }
    }
    return 0;
}

static bool block_state_has_block_entity(int32_t state_id) {
    if (state_id < 0) return false;

    if ((size_t)state_id < GLOBAL_BLOCK_STATES_COUNT) {
        return (GLOBAL_BLOCK_STATES[state_id].flags & MC_BLOCK_FLAG_HAS_BLOCK_ENTITY) != 0u;
    }

    const char *key = mc_block_state_key(state_id);
    if (!key) return false;

    const char *props = strchr(key, '[');
    size_t base_len = props ? (size_t)(props - key) : strlen(key);
    if (base_len == 0) return false;

    for (size_t i = 0; i < GLOBAL_BLOCK_COUNT; i++) {
        const mc_block_desc_t *desc = &GLOBAL_BLOCKS[i];
        if (!desc->name) continue;
        if (strlen(desc->name) != base_len) continue;
        if (strncmp(desc->name, key, base_len) != 0) continue;
        for (mc_global_state_id_t sid = desc->min_state_id; sid <= desc->max_state_id; sid++) {
            if ((size_t)sid < GLOBAL_BLOCK_STATES_COUNT &&
                (GLOBAL_BLOCK_STATES[sid].flags & MC_BLOCK_FLAG_HAS_BLOCK_ENTITY) != 0u) {
                return true;
            }
        }
        return false;
    }

    return false;
}

static const char *block_entity_type_name_for_state(int32_t state_id) {
    if (!block_state_has_block_entity(state_id)) return NULL;

    if ((size_t)state_id < GLOBAL_BLOCK_STATES_COUNT) {
        const mc_block_properties_t *props = &GLOBAL_BLOCK_STATES[state_id];
        if (props->block_index < GLOBAL_BLOCK_COUNT) {
            const mc_block_desc_t *desc = &GLOBAL_BLOCKS[props->block_index];
            if (desc->name && *desc->name && mc_minecraft_block_entity_type_id(desc->name) >= 0) return desc->name;
        }
    }

    const char *key = mc_block_state_key(state_id);
    if (!key) return NULL;
    const char *props = strchr(key, '[');
    static char name_buf[128];
    size_t base_len = props ? (size_t)(props - key) : strlen(key);
    if (base_len == 0 || base_len >= sizeof(name_buf)) return NULL;
    memcpy(name_buf, key, base_len);
    name_buf[base_len] = '\0';
    if (mc_minecraft_block_entity_type_id(name_buf) >= 0) return name_buf;
    if (is_ender_chest_state(state_id)) return "minecraft:ender_chest";
    if (is_trapped_chest_state(state_id)) return "minecraft:trapped_chest";
    if (is_chest_state(state_id)) return "minecraft:chest";
    if (is_barrel_state(state_id)) return "minecraft:barrel";
    if (is_furnace_state(state_id)) return "minecraft:furnace";
    if (is_smoker_state(state_id)) return "minecraft:smoker";
    if (is_blast_furnace_state(state_id)) return "minecraft:blast_furnace";
    if (is_dropper_state(state_id)) return "minecraft:dropper";
    if (is_shulker_box_state(state_id)) return "minecraft:shulker_box";
    return NULL;
}

static int container_block_entity_type(int32_t state_id) {
    const char *name = block_entity_type_name_for_state(state_id);
    return name ? mc_minecraft_block_entity_type_id(name) : -1;
}

static int32_t container_drop_item_id(int32_t state_id) {
    state_id = mc_world_normalize_container_state_id(state_id);
    if (is_chest_state(state_id)) return mc_minecraft_item_id("minecraft:chest");
    if (is_trapped_chest_state(state_id)) return mc_minecraft_item_id("minecraft:trapped_chest");
    if (is_ender_chest_state(state_id)) return mc_minecraft_item_id("minecraft:ender_chest");
    if (is_barrel_state(state_id)) return mc_minecraft_item_id("minecraft:barrel");
    if (is_furnace_state(state_id)) return mc_minecraft_item_id("minecraft:furnace");
    if (is_smoker_state(state_id)) return mc_minecraft_item_id("minecraft:smoker");
    if (is_blast_furnace_state(state_id)) return mc_minecraft_item_id("minecraft:blast_furnace");
    if (is_dropper_state(state_id)) return mc_minecraft_item_id("minecraft:dropper");
    if (is_shulker_box_state(state_id)) return mc_minecraft_item_id("minecraft:shulker_box");
    return -1;
}

static int32_t furnace_lit_state_id(int32_t state_id, bool lit) {
    if (!is_furnace_like_state(state_id)) return state_id;
    const char *key = mc_block_state_key(state_id);
    if (!key) return state_id;

    const char *lit_pos = strstr(key, "lit=");
    if (!lit_pos) return state_id;
    const char *after_lit = lit_pos + 4;
    while ((*after_lit >= 'a' && *after_lit <= 'z') || (*after_lit >= 'A' && *after_lit <= 'Z')) after_lit++;

    char next_key[192];
    size_t prefix_len = (size_t)(lit_pos - key);
    int n = snprintf(next_key, sizeof(next_key), "%.*slit=%s%s", (int)prefix_len, key, lit ? "true" : "false", after_lit);
    if (n <= 0 || (size_t)n >= sizeof(next_key)) return state_id;

    return mc_world_runtime_state_id_from_key(next_key, state_id);
}

static int update_open_furnace_lit_state(mc_conn_t *c, bool lit) {
    if (!c || !c->active_window.open || !c->active_window.container) return 0;
    mc_container_instance_t *container = c->active_window.container;
    if (!mc_furnace_container_kind_is_machine(container->kind)) return 0;

    mc_world_t *world = get_world(c);
    if (!world) return 0;
    int32_t current_state = -1;
    if (mc_world_get_block(world, container->x, container->y, container->z, &current_state) != 0) return -1;
    int32_t next_state = furnace_lit_state_id(current_state, lit);
    if (next_state < 0 || next_state == current_state) return 0;
    if (mc_world_set_block(world, container->x, container->y, container->z, next_state) != 0) return -1;
    return send_block_update_packet(c, container->x, container->y, container->z, next_state);
}

static int spawn_drop_slot(mc_conn_t *c, double x, double y, double z, const mc_slot_t *slot) {
    if (!c || !slot || !slot->present || slot->count <= 0) return 0;
    if (!c->server) return -1;
    return net_server_spawn_item_drop(c->server, x, y, z, slot);
}

static int spawn_drop_slot_locked(mc_conn_t *c, double x, double y, double z, const mc_slot_t *slot) {
    if (!c || !slot || !slot->present || slot->count <= 0) return 0;
    if (!c->server) return -1;
    return net_server_spawn_item_drop_locked(c->server, x, y, z, slot);
}

static int spawn_manual_drop_slot(mc_conn_t *c, double x, double y, double z, double vx, double vy, double vz, const mc_slot_t *slot) {
    if (!c || !slot || !slot->present || slot->count <= 0) return 0;
    if (!c->server) return -1;
    return net_server_spawn_item_drop_with_motion(c->server, x, y, z, vx, vy, vz, slot, PLAYER_MANUAL_DROP_PICKUP_DELAY_TICKS);
}

static int drop_player_slot(mc_conn_t *c, mc_slot_t *slot, bool server_locked) {
    if (!c || !slot || !slot->present || slot->count <= 0) return 0;
    int rc = server_locked ? spawn_drop_slot_locked(c, c->x, c->y + 0.5, c->z, slot) : spawn_drop_slot(c, c->x, c->y + 0.5, c->z, slot);
    if (rc != 0) return rc;
    mc_slot_clear(slot);
    return 1;
}

static int split_slot_for_drop(mc_slot_t *src, int32_t requested_count, mc_slot_t *out) {
    int32_t take = 0;

    if (!src || !out || requested_count <= 0) return 0;
    if (!src->present || src->count <= 0) return 0;

    take = src->count < requested_count ? src->count : requested_count;
    if (take <= 0) return 0;
    if (mc_slot_copy(out, src) != 0) return -1;
    out->count = take;
    src->count -= take;
    if (src->count <= 0) mc_slot_clear(src);
    return take;
}

static double wrap_radians(double angle) {
    const double pi = 3.14159265358979323846;
    const double tau = 6.28318530717958647692;

    while (angle > pi) angle -= tau;
    while (angle < -pi) angle += tau;
    return angle;
}

static double approx_sin(double angle) {
    const double pi = 3.14159265358979323846;
    const double b = 4.0 / pi;
    const double c = -4.0 / (pi * pi);
    const double p = 0.225;
    double x = wrap_radians(angle);
    double y = b * x + c * x * fabs(x);
    return p * (y * fabs(y) - y) + y;
}

static double approx_cos(double angle) {
    return approx_sin(angle + 1.57079632679489661923);
}

static void player_look_direction(const mc_conn_t *c, double *out_x, double *out_y, double *out_z) {
    const double deg_to_rad = 3.14159265358979323846 / 180.0;
    double yaw_rad = 0.0;
    double pitch_rad = 0.0;
    double dx = 0.0;
    double dy = 0.0;
    double dz = 1.0;

    if (!c) {
        if (out_x) *out_x = 0.0;
        if (out_y) *out_y = 0.0;
        if (out_z) *out_z = 1.0;
        return;
    }

    yaw_rad = (double)c->yaw * deg_to_rad;
    pitch_rad = (double)c->pitch * deg_to_rad;
    dx = -approx_sin(yaw_rad) * approx_cos(pitch_rad);
    dy = -approx_sin(pitch_rad);
    dz = approx_cos(yaw_rad) * approx_cos(pitch_rad);

    if (out_x) *out_x = dx;
    if (out_y) *out_y = dy;
    if (out_z) *out_z = dz;
}

static void manual_drop_spawn_motion(const mc_conn_t *c, double *out_x, double *out_y, double *out_z, double *out_vx, double *out_vy,
                                     double *out_vz) {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double dir_x = 0.0;
    double dir_y = 0.0;
    double dir_z = 1.0;
    double vx = 0.0;
    double vy = PLAYER_DROP_UPWARD_SPEED;
    double vz = 0.0;

    if (!c) {
        if (out_x) *out_x = 0.0;
        if (out_y) *out_y = 0.0;
        if (out_z) *out_z = 0.0;
        if (out_vx) *out_vx = 0.0;
        if (out_vy) *out_vy = PLAYER_DROP_UPWARD_SPEED;
        if (out_vz) *out_vz = 0.0;
        return;
    }

    player_look_direction(c, &dir_x, &dir_y, &dir_z);
    x = c->x + dir_x * PLAYER_DROP_FORWARD_OFFSET;
    y = c->y + PLAYER_DROP_VERTICAL_OFFSET + dir_y * PLAYER_DROP_FORWARD_OFFSET;
    z = c->z + dir_z * PLAYER_DROP_FORWARD_OFFSET;
    vx = dir_x * PLAYER_DROP_FORWARD_SPEED;
    vy = PLAYER_DROP_UPWARD_SPEED + dir_y * PLAYER_DROP_FORWARD_SPEED;
    vz = dir_z * PLAYER_DROP_FORWARD_SPEED;
    if (vy < 0.05) vy = 0.05;

    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
    if (out_z) *out_z = z;
    if (out_vx) *out_vx = vx;
    if (out_vy) *out_vy = vy;
    if (out_vz) *out_vz = vz;
}

static int drop_player_inventory(mc_conn_t *c, bool server_locked) {
    mc_inventory_t *inv = conn_inventory(c);
    int dropped = 0;

    if (!c || !inv) return -1;
    mc_crafting_clear_result(&inv->slots[PLAYER_CRAFTING_RESULT_SLOT]);
    for (int i = 0; i < MC_PLAYER_SLOT_COUNT; i++) {
        int rc = drop_player_slot(c, &inv->slots[i], server_locked);
        if (rc < 0) return -1;
        if (rc > 0) dropped += rc;
    }
    {
        int rc = drop_player_slot(c, &inv->cursor_slot, server_locked);
        if (rc < 0) return -1;
        if (rc > 0) dropped += rc;
    }
    if (dropped > 0) inv->state_id++;
    return dropped;
}

static int handle_player_death(mc_conn_t *c, const char *death_message, bool server_locked) {
    if (!c || !c->player) return -1;
    if (c->dead) return 0;

    c->health = 0.0f;
    c->player->health = 0.0f;
    c->dead = true;
    c->next_natural_regen_ms = 0;
    c->next_starvation_damage_ms = 0;
    cancel_item_use(c);
    reset_fall_tracking(c);

    close_active_window(c, true);
    if (drop_player_inventory(c, server_locked) < 0) return -1;
    if (sync_inventory_full(c) != 0) return -1;
    if (send_set_health_packet(c) != 0) return -1;
    if (send_combat_kill_packet(c, death_message) != 0) return -1;
    return save_player_data(c);
}

static int apply_player_damage(mc_conn_t *c, float amount, const char *death_message, bool server_locked, int64_t now_ms) {
    (void)now_ms;
    if (!player_can_take_damage(c)) return 0;
    if (!(amount > 0.0f)) return 0;

    c->health -= amount;
    if (c->health < 0.0f) c->health = 0.0f;
    c->player->health = c->health;

    if (c->health <= 0.0f) {
        return handle_player_death(c, death_message, server_locked);
    }
    if (send_set_health_packet(c) != 0) return -1;
    return 0;
}

static int player_set_health_debug(mc_conn_t *c, float health, const char *death_message) {
    if (!c || !c->player) return -1;
    health = clamp_player_health(health);
    if (health <= 0.0f) {
        return handle_player_death(c, death_message ? death_message : "Killed by debug command", false);
    }
    if (c->dead) return 0;
    c->health = health;
    c->player->health = health;
    if (send_set_health_packet(c) != 0) return -1;
    return save_player_data(c);
}

static int player_heal(mc_conn_t *c, float amount) {
    if (!c || !c->player || c->dead) return 0;
    if (!(amount > 0.0f) || !isfinite(amount)) return 0;
    if (c->health >= PLAYER_MAX_HEALTH) return 0;

    c->health = clamp_player_health(c->health + amount);
    c->player->health = c->health;
    return send_set_health_packet(c);
}

static float starvation_min_health_for_difficulty(mc_difficulty_t difficulty) {
    switch (difficulty) {
        case MC_DIFFICULTY_EASY:
            return 10.0f;
        case MC_DIFFICULTY_NORMAL:
            return 1.0f;
        case MC_DIFFICULTY_HARD:
            return 0.0f;
        case MC_DIFFICULTY_PEACEFUL:
        default:
            return PLAYER_MAX_HEALTH;
    }
}

static int tick_natural_regen_and_starvation(mc_conn_t *c, int64_t now_ms) {
    if (!player_can_use_hunger_system(c)) {
        if (c) {
            c->next_natural_regen_ms = 0;
            c->next_starvation_damage_ms = 0;
        }
        return 0;
    }

    mc_difficulty_t difficulty = current_difficulty(c);
    c->food = clamp_food_level(c->food);
    c->food_saturation = clamp_food_saturation(c->food, c->food_saturation);
    c->food_exhaustion = clamp_food_exhaustion(c->food_exhaustion);

    if (difficulty == MC_DIFFICULTY_PEACEFUL) {
        c->food_exhaustion = 0.0f;
        c->player->food_exhaustion = 0.0f;
        c->next_starvation_damage_ms = 0;

        if (c->health < PLAYER_MAX_HEALTH) {
            if (c->next_natural_regen_ms == 0) c->next_natural_regen_ms = now_ms + PLAYER_PEACEFUL_REGEN_INTERVAL_MS;
            if (now_ms >= c->next_natural_regen_ms) {
                c->next_natural_regen_ms = now_ms + PLAYER_PEACEFUL_REGEN_INTERVAL_MS;
                return player_heal(c, PLAYER_NATURAL_REGEN_AMOUNT);
            }
        } else {
            c->next_natural_regen_ms = 0;
        }
        return 0;
    }

    if (player_apply_exhaustion_thresholds(c, false) != 0) return -1;

    if (c->health < PLAYER_MAX_HEALTH && c->food >= PLAYER_NATURAL_REGEN_FOOD_THRESHOLD) {
        if (c->next_natural_regen_ms == 0) c->next_natural_regen_ms = now_ms + PLAYER_NATURAL_REGEN_INTERVAL_MS;
        if (now_ms >= c->next_natural_regen_ms) {
            c->next_natural_regen_ms = now_ms + PLAYER_NATURAL_REGEN_INTERVAL_MS;
            if (player_heal(c, PLAYER_NATURAL_REGEN_AMOUNT) != 0) return -1;
            if (player_add_exhaustion(c, PLAYER_NATURAL_REGEN_EXHAUSTION, false) != 0) return -1;
        }
    } else {
        c->next_natural_regen_ms = 0;
    }

    if (c->food <= 0 && difficulty != MC_DIFFICULTY_PEACEFUL) {
        float min_health = starvation_min_health_for_difficulty(difficulty);
        if (c->health > min_health) {
            if (c->next_starvation_damage_ms == 0) c->next_starvation_damage_ms = now_ms + PLAYER_STARVATION_DAMAGE_INTERVAL_MS;
            if (now_ms >= c->next_starvation_damage_ms) {
                float amount = PLAYER_STARVATION_DAMAGE_AMOUNT;
                float allowed_damage = c->health - min_health;
                if (amount > allowed_damage) amount = allowed_damage;
                c->next_starvation_damage_ms = now_ms + PLAYER_STARVATION_DAMAGE_INTERVAL_MS;
                if (amount > 0.0f) {
                    return apply_player_damage(c, amount, "You starved to death", true, now_ms);
                }
            }
        } else {
            c->next_starvation_damage_ms = 0;
        }
    } else {
        c->next_starvation_damage_ms = 0;
    }

    return 0;
}

static int break_container_block(mc_conn_t *c, int32_t x, int32_t y, int32_t z, int32_t state_id) {
    if (!c) return -1;
    mc_world_t *world = get_world(c);
    const mc_world_ids_t *ids = mc_world_ids(world);
    if (!world || !ids) return -1;

    state_id = mc_world_normalize_container_state_id(state_id);
    bool is_ender = is_ender_chest_state(state_id);
    bool is_normal = is_world_container_state(state_id);
    mc_container_kind_t container_kind = container_kind_for_state(state_id);
    if (!is_ender && !is_normal) return 1;

    mc_container_instance_t container;
    bool have_container = false;
    if (is_normal) {
        int rc = net_server_get_open_container_snapshot(c->server, container_kind, x, y, z, &container);
        if (rc == 0) {
            have_container = true;
        } else if (rc == 1) {
            mc_block_entity_t *entity = mc_world_get_block_entity(world, x, y, z);
            if (entity) {
                container_instance_from_block_entity(&container, container_kind, x, y, z, entity);
                have_container = true;
            }
        } else {
            return -1;
        }
    }

    net_server_close_container_viewers(c->server, is_ender ? MC_CONTAINER_KIND_ENDER_CHEST : container_kind, x, y, z);

    if (mc_world_set_block(world, x, y, z, ids->air) != 0) {
        if (have_container) mc_container_instance_clear(&container);
        return -1;
    }
    (void)mc_world_remove_block_entity(world, x, y, z);
    if (mc_world_flush_block(world, x, y, z) != 0) {
        if (have_container) mc_container_instance_clear(&container);
        return -1;
    }

    int32_t chest_item_id = container_drop_item_id(state_id);
    if (chest_item_id >= 0) {
        mc_slot_t block_drop = {0};
        if (mc_slot_set_simple(&block_drop, chest_item_id, 1) == 0) {
            if (spawn_drop_slot(c, x + 0.5, y + 0.5, z + 0.5, &block_drop) != 0) {
                mc_slot_clear(&block_drop);
                if (have_container) mc_container_instance_clear(&container);
                return -1;
            }
        }
        mc_slot_clear(&block_drop);
    }

    if (is_normal && have_container) {
        int slot_count = container.slot_count > 0 ? container.slot_count : MC_CONTAINER_SLOT_COUNT;
        if (slot_count > MC_CONTAINER_SLOT_COUNT) slot_count = MC_CONTAINER_SLOT_COUNT;
        for (int i = 0; i < slot_count; i++) {
            if (!container.slots[i].present || container.slots[i].count <= 0) continue;
            if (spawn_drop_slot(c, x + 0.5, y + 0.5, z + 0.5, &container.slots[i]) != 0) {
                mc_container_instance_clear(&container);
                return -1;
            }
            mc_slot_clear(&container.slots[i]);
        }
            mc_container_instance_clear(&container);
    }
    if (resend_authoritative_chunk_at(c, world, x, z) != 0) {
        return -1;
    }
    return 0;
}

static int reject_block_destroy(mc_conn_t *c, int32_t x, int32_t y, int32_t z, int32_t state_id, int32_t seq) {
    if (state_id >= 0 && send_block_update_packet(c, x, y, z, state_id) != 0) return -1;
    return send_block_changed_ack_packet(c, seq);
}

static int break_block_authoritative(mc_conn_t *c, mc_world_t *world, const mc_world_ids_t *ids, int32_t x, int32_t y,
                                     int32_t z, int32_t state_id, int32_t seq, bool allow_default_drop) {
    if (!c || !world || !ids) return -1;

    int brc = break_container_block(c, x, y, z, state_id);
    if (brc < 0) return -1;
    if (brc == 0) {
        if (send_block_update_packet(c, x, y, z, ids->air) != 0) return -1;
        return send_block_changed_ack_packet(c, seq);
    }

    if (!mc_mining_state_is_air(state_id)) {
        int32_t drop_item_id = allow_default_drop ? mc_block_loot_default_item_id_from_state(state_id, -1) : -1;
        (void)mc_world_remove_block_entity(world, x, y, z);
        if (mc_world_set_block(world, x, y, z, ids->air) != 0) return -1;
        if (mc_world_flush_block(world, x, y, z) != 0) return -1;
        if (send_block_update_packet(c, x, y, z, ids->air) != 0) return -1;

        if (drop_item_id >= 0) {
            mc_slot_t block_drop = {0};
            if (mc_slot_set_simple(&block_drop, drop_item_id, 1) == 0) {
                if (spawn_drop_slot(c, x + 0.5, y + 0.5, z + 0.5, &block_drop) != 0) {
                    mc_slot_clear(&block_drop);
                    return -1;
                }
            }
            mc_slot_clear(&block_drop);
        }
        return send_block_changed_ack_packet(c, seq);
    }

    if (send_block_update_packet(c, x, y, z, state_id) != 0) return -1;
    return send_block_changed_ack_packet(c, seq);
}

static int try_open_target_container(mc_conn_t *c, int32_t x, int32_t y, int32_t z) {
    if (!c || !c->player) return 0;
    mc_world_t *world = get_world(c);
    if (!world) return 0;

    int32_t state_id = -1;
    if (mc_world_get_block(world, x, y, z, &state_id) != 0) return 0;
    int32_t normalized_state = mc_world_normalize_container_state_id(state_id);
    if (normalized_state != state_id) {
        (void)mc_world_set_block(world, x, y, z, normalized_state);
        (void)mc_world_flush_block(world, x, y, z);
        (void)resend_authoritative_chunk_at(c, world, x, z);
        if (debug_container_pos_match(x, y, z)) {
            log_info("containers debug: open normalize pos=(%d,%d,%d) old=%d new=%d old_key=%s new_key=%s", x, y, z, state_id,
                     normalized_state, mc_block_state_key(state_id) ? mc_block_state_key(state_id) : "(null)",
                     mc_block_state_key(normalized_state) ? mc_block_state_key(normalized_state) : "(null)");
        }
    }
    state_id = normalized_state;

    mc_container_instance_t *container = NULL;
    const char *title_key = NULL;
    if (is_crafting_table_state(state_id)) {
        container = (mc_container_instance_t *)calloc(1, sizeof(*container));
        if (!container) return -1;
        mc_container_instance_init(container, MC_CONTAINER_KIND_CRAFTING_TABLE, x, y, z);
        container->slot_count = CRAFTING_TABLE_SLOT_COUNT;
        title_key = "container.crafting";
    } else if (is_world_container_state(state_id)) {
        container = (mc_container_instance_t *)calloc(1, sizeof(*container));
        if (!container) return -1;
        mc_container_kind_t kind = container_kind_for_state(state_id);
        mc_block_entity_t *entity = mc_world_get_block_entity(world, x, y, z);
        if (entity) {
            container_instance_from_block_entity(container, kind, x, y, z, entity);
        } else {
            mc_container_instance_init(container, kind, x, y, z);
            mc_block_entity_t fresh;
            if (block_entity_from_container_instance(&fresh, container_entity_type_for_state(state_id), container) != 0 ||
                mc_world_put_block_entity(world, x, y, z, &fresh) != 0) {
                mc_container_instance_clear(container);
                free(container);
                return -1;
            }
        }
        title_key = container_title_for_state(state_id);
    } else if (is_ender_chest_state(state_id)) {
        container = (mc_container_instance_t *)calloc(1, sizeof(*container));
        if (!container) return -1;
        mc_container_instance_init(container, MC_CONTAINER_KIND_ENDER_CHEST, x, y, z);
        container->state_id = c->player->ender_state_id > 0 ? c->player->ender_state_id : 1;
        for (int i = 0; i < MC_CONTAINER_SLOT_COUNT; i++) {
            if (mc_slot_copy(&container->slots[i], &c->player->ender_chest[i]) != 0) {
                mc_container_instance_clear(container);
                free(container);
                return -1;
            }
        }
        title_key = "container.enderchest";
    } else {
        return 0;
    }

    if (debug_place_enabled()) {
        log_info("place debug: open container kind=%d pos=(%d,%d,%d) state_id=%d key=%s", (int)container->kind, x, y, z, state_id,
                 mc_block_state_key(state_id) ? mc_block_state_key(state_id) : "(null)");
    }
    int32_t window_type = window_type_for_container_kind(container->kind);
    if (open_container_window_typed(c, container, window_type, title_key) != 0) {
        mc_container_instance_clear(container);
        free(container);
        return -1;
    }
    if (is_shulker_box_state(state_id)) {
        if (send_block_entity_data_packet(c, state_id, x, y, z) != 0 ||
            send_block_event_packet(c, x, y, z, 1, 1, block_action_block_id_for_state(state_id)) != 0) {
            return -1;
        }
    }
    return 1;
}

static uint8_t angle_byte(float degrees) {
    float norm = degrees;
    while (norm < 0.0f) norm += 360.0f;
    while (norm >= 360.0f) norm -= 360.0f;
    int v = (int)(norm * 256.0f / 360.0f + 0.5f);
    return (uint8_t)(v & 0xFF);
}

static double abs_d(double v) {
    return v < 0.0 ? -v : v;
}

static mc_remote_player_t *remote_player_get(mc_conn_t *viewer, int32_t entity_id) {
    if (!viewer) return NULL;
    for (size_t i = 0; i < viewer->remote_players_len; i++) {
        if (viewer->remote_players[i].entity_id == entity_id) return &viewer->remote_players[i];
    }
    return NULL;
}

static mc_remote_player_t *remote_player_ensure(mc_conn_t *viewer, int32_t entity_id, const uint8_t uuid[16]) {
    if (!viewer) return NULL;
    mc_remote_player_t *entry = remote_player_get(viewer, entity_id);
    if (entry) return entry;
    if (viewer->remote_players_len == viewer->remote_players_cap) {
        size_t new_cap = viewer->remote_players_cap ? viewer->remote_players_cap * 2 : 8;
        mc_remote_player_t *next =
            (mc_remote_player_t *)realloc(viewer->remote_players, new_cap * sizeof(*next));
        if (!next) return NULL;
        viewer->remote_players = next;
        viewer->remote_players_cap = new_cap;
    }
    entry = &viewer->remote_players[viewer->remote_players_len++];
    memset(entry, 0, sizeof(*entry));
    entry->entity_id = entity_id;
    if (uuid) memcpy(entry->uuid, uuid, 16);
    return entry;
}

static void remote_player_remove_local(mc_conn_t *viewer, int32_t entity_id) {
    if (!viewer) return;
    for (size_t i = 0; i < viewer->remote_players_len; i++) {
        if (viewer->remote_players[i].entity_id != entity_id) continue;
        size_t last = viewer->remote_players_len - 1;
        if (i != last) viewer->remote_players[i] = viewer->remote_players[last];
        viewer->remote_players_len--;
        return;
    }
}

static int send_player_info_add(mc_conn_t *viewer, mc_conn_t *subject) {
    uint8_t buf[512];
    size_t pos = 0;
    uint8_t action = PLAYER_INFO_ACTION_ADD | PLAYER_INFO_ACTION_UPDATE_GAMEMODE | PLAYER_INFO_ACTION_UPDATE_LISTED |
                     PLAYER_INFO_ACTION_UPDATE_LATENCY;
    if (w_ubyte(buf, sizeof(buf), &pos, action) != 0) return -1;
    if (w_varint(buf, sizeof(buf), &pos, 1) != 0) return -1;
    if (pos + 16 > sizeof(buf)) return -1;
    memcpy(buf + pos, subject->uuid, 16);
    pos += 16;
    if (w_string(buf, sizeof(buf), &pos, subject->username) != 0) return -1;
    if (w_varint(buf, sizeof(buf), &pos, 0) != 0) return -1; /* properties */
    if (w_varint(buf, sizeof(buf), &pos, subject->gamemode) != 0) return -1;
    if (w_varint(buf, sizeof(buf), &pos, 1) != 0) return -1; /* listed */
    if (w_varint(buf, sizeof(buf), &pos, 0) != 0) return -1; /* latency */
    return conn_write_packet(viewer, PKT_PLAY_PLAYER_INFO, buf, pos, -1);
}

static int send_player_remove(mc_conn_t *viewer, mc_conn_t *subject) {
    uint8_t buf[32];
    size_t pos = 0;
    if (w_varint(buf, sizeof(buf), &pos, 1) != 0) return -1;
    if (pos + 16 > sizeof(buf)) return -1;
    memcpy(buf + pos, subject->uuid, 16);
    pos += 16;
    return conn_write_packet(viewer, PKT_PLAY_PLAYER_REMOVE, buf, pos, -1);
}

static int send_player_spawn(mc_conn_t *viewer, mc_conn_t *subject) {
    uint8_t buf[128];
    size_t pos = 0;
    int player_entity_type_id = resolve_player_entity_type_id();
    if (player_entity_type_id < 0) return -1;
    if (debug_players_enabled()) {
        log_info("players debug: spawn entity_id=%d uuid=%02x%02x.. type_id=%d type=%s", subject->entity_id,
                 subject->uuid[0], subject->uuid[1], player_entity_type_id,
                 mc_minecraft_entity_type_name(player_entity_type_id) ? mc_minecraft_entity_type_name(player_entity_type_id) : "(unknown)");
    }
    if (w_varint(buf, sizeof(buf), &pos, subject->entity_id) != 0) return -1;
    if (pos + 16 > sizeof(buf)) return -1;
    memcpy(buf + pos, subject->uuid, 16);
    pos += 16;
    if (w_varint(buf, sizeof(buf), &pos, player_entity_type_id) != 0) return -1;
    if (w_f64(buf, sizeof(buf), &pos, subject->x) != 0 || w_f64(buf, sizeof(buf), &pos, subject->y) != 0 ||
        w_f64(buf, sizeof(buf), &pos, subject->z) != 0) {
        return -1;
    }
    if (w_lpvec3_zero(buf, sizeof(buf), &pos) != 0) return -1;
    if (w_byte(buf, sizeof(buf), &pos, (int8_t)angle_byte(subject->pitch)) != 0 ||
        w_byte(buf, sizeof(buf), &pos, (int8_t)angle_byte(subject->yaw)) != 0 ||
        w_byte(buf, sizeof(buf), &pos, (int8_t)angle_byte(subject->pitch)) != 0) {
        return -1;
    }
    if (w_varint(buf, sizeof(buf), &pos, 0) != 0) return -1;
    return conn_write_packet(viewer, PKT_PLAY_SPAWN_ENTITY, buf, pos, -1);
}

static int send_player_head_rotation(mc_conn_t *viewer, mc_conn_t *subject) {
    uint8_t buf[16];
    size_t pos = 0;
    if (w_varint(buf, sizeof(buf), &pos, subject->entity_id) != 0) return -1;
    if (w_byte(buf, sizeof(buf), &pos, (int8_t)angle_byte(subject->yaw)) != 0) return -1;
    return conn_write_packet(viewer, PKT_PLAY_ENTITY_HEAD_ROTATION, buf, pos, -1);
}

static int send_player_teleport(mc_conn_t *viewer, mc_conn_t *subject) {
    uint8_t buf[96];
    size_t pos = 0;
    if (w_varint(buf, sizeof(buf), &pos, subject->entity_id) != 0) return -1;
    /* 26.1 ClientboundTeleportEntityPacket = entity id + PositionMoveRotation + Relative set + onGround.
     * PositionMoveRotation carries absolute position, absolute delta movement, then yaw/pitch. */
    if (w_f64(buf, sizeof(buf), &pos, subject->x) != 0 || w_f64(buf, sizeof(buf), &pos, subject->y) != 0 ||
        w_f64(buf, sizeof(buf), &pos, subject->z) != 0) {
        return -1;
    }
    if (w_f64(buf, sizeof(buf), &pos, 0.0) != 0 || w_f64(buf, sizeof(buf), &pos, 0.0) != 0 ||
        w_f64(buf, sizeof(buf), &pos, 0.0) != 0) {
        return -1;
    }
    if (w_f32(buf, sizeof(buf), &pos, subject->yaw) != 0 ||
        w_f32(buf, sizeof(buf), &pos, subject->pitch) != 0 ||
        w_i32(buf, sizeof(buf), &pos, 0) != 0 ||
        w_bool(buf, sizeof(buf), &pos, subject->on_ground) != 0) {
        return -1;
    }
    return conn_write_packet(viewer, PKT_PLAY_ENTITY_TELEPORT, buf, pos, -1);
}

static int hide_remote_player_entity(mc_conn_t *viewer, mc_conn_t *subject) {
    if (!viewer || !subject || viewer == subject) return 0;
    mc_remote_player_t *entry = remote_player_get(viewer, subject->entity_id);
    if (!entry) return 0;

    if (entry->spawned) {
        uint8_t buf[16];
        size_t pos = 0;
        if (w_varint(buf, sizeof(buf), &pos, 1) != 0 || w_varint(buf, sizeof(buf), &pos, subject->entity_id) != 0) return -1;
        if (conn_write_packet(viewer, PKT_PLAY_ENTITY_DESTROY, buf, pos, -1) != 0) return -1;
        if (debug_players_enabled()) {
            log_info("players debug: hide dead viewer=%s subject=%s eid=%d", viewer->username, subject->username, subject->entity_id);
        }
    }

    entry->spawned = false;
    entry->has_last_pos = false;
    return 0;
}

static int send_player_move_update(mc_conn_t *viewer, mc_conn_t *subject, const mc_remote_player_t *entry) {
    if (!viewer || !subject || !entry || !entry->has_last_pos) return -1;
    double dx = subject->x - entry->x;
    double dy = subject->y - entry->y;
    double dz = subject->z - entry->z;
    int16_t mx = (int16_t)((dx >= 0.0) ? (dx * 4096.0 + 0.5) : (dx * 4096.0 - 0.5));
    int16_t my = (int16_t)((dy >= 0.0) ? (dy * 4096.0 + 0.5) : (dy * 4096.0 - 0.5));
    int16_t mz = (int16_t)((dz >= 0.0) ? (dz * 4096.0 + 0.5) : (dz * 4096.0 - 0.5));
    bool pos_changed = abs_d(dx) > 0.0001 || abs_d(dy) > 0.0001 || abs_d(dz) > 0.0001;
    bool look_changed = angle_byte(subject->yaw) != angle_byte(entry->yaw) || angle_byte(subject->pitch) != angle_byte(entry->pitch);
    bool need_teleport =
        (!entry->spawned) || abs_d(dx * 4096.0) > 32767.0 || abs_d(dy * 4096.0) > 32767.0 || abs_d(dz * 4096.0) > 32767.0;
    if (need_teleport) return send_player_teleport(viewer, subject);
    if (!pos_changed && !look_changed) return 0;

    uint8_t buf[32];
    size_t pos = 0;
    if (w_varint(buf, sizeof(buf), &pos, subject->entity_id) != 0) return -1;
    int packet_id = PKT_PLAY_ENTITY_LOOK;
    if (pos_changed) {
        if (w_u16_be(buf, sizeof(buf), &pos, (uint16_t)mx) != 0 || w_u16_be(buf, sizeof(buf), &pos, (uint16_t)my) != 0 ||
            w_u16_be(buf, sizeof(buf), &pos, (uint16_t)mz) != 0) {
            return -1;
        }
        packet_id = look_changed ? PKT_PLAY_ENTITY_MOVE_LOOK : PKT_PLAY_REL_ENTITY_MOVE;
    }
    if (look_changed) {
        if (w_byte(buf, sizeof(buf), &pos, (int8_t)angle_byte(subject->yaw)) != 0 ||
            w_byte(buf, sizeof(buf), &pos, (int8_t)angle_byte(subject->pitch)) != 0) {
            return -1;
        }
        if (!pos_changed) packet_id = PKT_PLAY_ENTITY_LOOK;
    }
    if (w_bool(buf, sizeof(buf), &pos, true) != 0) return -1;
    return conn_write_packet(viewer, packet_id, buf, pos, -1);
}

int proto_play_sync_remote_player(mc_conn_t *viewer, mc_conn_t *subject) {
    if (!viewer || !subject || viewer == subject || viewer->closing || subject->closing || !viewer->play_ready || !subject->play_ready ||
        !subject->has_pos) {
        return 0;
    }

    if (subject->dead) {
        return hide_remote_player_entity(viewer, subject);
    }

    mc_remote_player_t *entry = remote_player_ensure(viewer, subject->entity_id, subject->uuid);
    if (!entry) return -1;

    if (!entry->listed) {
        if (send_player_info_add(viewer, subject) != 0) return -1;
        entry->listed = true;
        if (debug_players_enabled()) log_info("players debug: info add viewer=%s subject=%s", viewer->username, subject->username);
    }
    if (!entry->spawned) {
        if (send_player_spawn(viewer, subject) != 0 || send_player_head_rotation(viewer, subject) != 0 ||
            send_player_teleport(viewer, subject) != 0) {
            return -1;
        }
        entry->spawned = true;
        if (debug_players_enabled()) {
            log_info("players debug: spawn viewer=%s subject=%s eid=%d uuid=%02x%02x%02x%02x type=%d", viewer->username,
                     subject->username, subject->entity_id, subject->uuid[0], subject->uuid[1], subject->uuid[2], subject->uuid[3],
                     resolve_player_entity_type_id());
        }
    } else {
        if (send_player_move_update(viewer, subject, entry) != 0) return -1;
        if (angle_byte(subject->yaw) != angle_byte(entry->head_yaw)) {
            if (send_player_head_rotation(viewer, subject) != 0) return -1;
        }
    }

    entry->has_last_pos = true;
    entry->x = subject->x;
    entry->y = subject->y;
    entry->z = subject->z;
    entry->yaw = subject->yaw;
    entry->pitch = subject->pitch;
    entry->head_yaw = subject->yaw;
    memcpy(entry->uuid, subject->uuid, 16);
    return 0;
}

int proto_play_remove_remote_player(mc_conn_t *viewer, mc_conn_t *subject) {
    if (!viewer || !subject || viewer == subject) return 0;
    mc_remote_player_t *entry = remote_player_get(viewer, subject->entity_id);
    if (!entry) return 0;

    if (entry->spawned) {
        uint8_t buf[16];
        size_t pos = 0;
        if (w_varint(buf, sizeof(buf), &pos, 1) != 0 || w_varint(buf, sizeof(buf), &pos, subject->entity_id) != 0) return -1;
        if (conn_write_packet(viewer, PKT_PLAY_ENTITY_DESTROY, buf, pos, -1) != 0) return -1;
        if (debug_players_enabled()) log_info("players debug: destroy viewer=%s subject=%s eid=%d", viewer->username, subject->username, subject->entity_id);
    }
    if (entry->listed) {
        if (send_player_remove(viewer, subject) != 0) return -1;
        if (debug_players_enabled()) log_info("players debug: info remove viewer=%s subject=%s", viewer->username, subject->username);
    }
    remote_player_remove_local(viewer, subject->entity_id);
    return 0;
}

static const mc_slot_t *selected_mainhand_slot(const mc_conn_t *c) {
    const mc_inventory_t *inv = conn_inventory_const(c);
    return inv ? mc_inventory_selected_slot_const(inv) : NULL;
}

static int selected_mainhand_slot_index(const mc_conn_t *c) {
    const mc_inventory_t *inv = conn_inventory_const(c);
    return inv ? mc_inventory_selected_slot_index(inv) : -1;
}

static int send_local_player_use_item_metadata(mc_conn_t *c, bool active, int32_t hand) {
    uint8_t buf[16];
    size_t pos = 0;
    uint8_t flags = 0;

    if (!c) return -1;
    if (active) {
        flags |= LIVING_ENTITY_FLAG_USING_ITEM;
        if (hand != 0) flags |= LIVING_ENTITY_FLAG_USING_OFFHAND;
    }
    if (w_varint(buf, sizeof(buf), &pos, c->entity_id) != 0) return -1;
    if (w_ubyte(buf, sizeof(buf), &pos, PLAYER_METADATA_LIVING_FLAGS_INDEX) != 0) return -1;
    if (w_varint(buf, sizeof(buf), &pos, ENTITY_METADATA_TYPE_BYTE) != 0) return -1;
    if (w_byte(buf, sizeof(buf), &pos, (int8_t)flags) != 0) return -1;
    if (w_ubyte(buf, sizeof(buf), &pos, 0xFF) != 0) return -1;
    return conn_write_packet(c, PKT_PLAY_ENTITY_METADATA, buf, pos, -1);
}

static int stop_active_item_use(mc_conn_t *c) {
    bool had_active = false;

    if (!c) return -1;
    had_active = c->is_using_item;
    clear_active_item_use(c);
    if (!had_active) return 0;
    return send_local_player_use_item_metadata(c, false, -1);
}

static void clear_active_item_use(mc_conn_t *c) {
    if (!c) return;
    c->is_using_item = false;
    c->using_hand = -1;
    c->using_slot = -1;
    c->using_item_id = -1;
    c->use_item_remaining_ticks = 0;
}

static void cancel_item_use(mc_conn_t *c) {
    bool had_active = false;

    if (!c) return;
    had_active = c->is_using_item;
    clear_active_item_use(c);
    c->use_item_input_held = false;
    if (had_active && send_local_player_use_item_metadata(c, false, -1) != 0) {
        conn_close(c);
    }
}

static int consume_selected_item(mc_conn_t *c) {
    mc_inventory_t *inv = conn_inventory(c);
    if (!inv) return -1;
    int idx = mc_inventory_selected_slot_index(inv);
    if (idx < 0 || idx >= MC_PLAYER_SLOT_COUNT) return -1;
    mc_slot_t *slot = &inv->slots[idx];
    if (!slot->present || slot->count <= 0) return -1;
    slot->count--;
    if (slot->count <= 0) mc_slot_clear(slot);
    inv->state_id++;
    if (debug_place_enabled()) {
        const char *item_name = (slot && slot->present) ? mc_minecraft_item_name(slot->item_id) : NULL;
        log_info("place debug: consume slot=%d remaining=%d item_id=%d item=%s", idx, slot->present ? slot->count : 0,
                 slot->present ? slot->item_id : -1, item_name ? item_name : "(empty)");
    }
    if (sync_inventory_slot(c, (int16_t)idx) != 0) return -1;
    return save_player_data(c);
}

static bool active_food_use_matches(mc_conn_t *c, const mc_slot_t **out_slot, const mc_item_food_entry_t **out_food) {
    const mc_slot_t *slot = NULL;
    const mc_item_food_entry_t *food = NULL;

    if (out_slot) *out_slot = NULL;
    if (out_food) *out_food = NULL;
    if (!c || !c->player || !c->is_using_item || c->using_hand != 0) return false;
    if (c->using_slot < 0 || c->using_slot >= MC_PLAYER_SLOT_COUNT) return false;
    if (selected_mainhand_slot_index(c) != c->using_slot) return false;

    slot = selected_mainhand_slot(c);
    if (!slot || !slot->present || slot->count <= 0) return false;
    if (slot->item_id != c->using_item_id) return false;

    food = mc_item_food_entry(slot->item_id);
    if (!food || !(food->flags & MC_ITEM_FOOD_FLAG_PRESENT)) return false;

    c->food = clamp_food_level(c->food);
    c->food_saturation = clamp_food_saturation(c->food, c->food_saturation);
    c->food_exhaustion = clamp_food_exhaustion(c->food_exhaustion);
    if (c->food >= PLAYER_MAX_FOOD_LEVEL && !(food->flags & MC_ITEM_FOOD_FLAG_ALWAYS_EDIBLE)) return false;

    if (out_slot) *out_slot = slot;
    if (out_food) *out_food = food;
    return true;
}

static int start_food_use_cycle(mc_conn_t *c, int32_t hand, int32_t slot_index, int32_t item_id) {
    if (!c) return -1;
    clear_active_item_use(c);
    c->is_using_item = true;
    c->using_hand = hand;
    c->using_slot = slot_index;
    c->using_item_id = item_id;
    c->use_item_remaining_ticks = PLAYER_FOOD_USE_DURATION_TICKS;
    return send_local_player_use_item_metadata(c, true, hand);
}

static int maybe_restart_food_use_cycle(mc_conn_t *c) {
    const mc_slot_t *slot = NULL;
    const mc_item_food_entry_t *food = NULL;
    int32_t idx = -1;

    if (!c) return -1;
    if (!c->use_item_input_held) {
        clear_active_item_use(c);
        return 0;
    }
    if (!player_can_use_hunger_system(c) || c->dead) {
        cancel_item_use(c);
        return 0;
    }

    slot = selected_mainhand_slot(c);
    idx = selected_mainhand_slot_index(c);
    if (!slot || !slot->present || slot->count <= 0) {
        cancel_item_use(c);
        return 0;
    }
    if (idx < 0 || idx >= MC_PLAYER_SLOT_COUNT) {
        cancel_item_use(c);
        return 0;
    }
    food = mc_item_food_entry(slot->item_id);
    if (!food || !(food->flags & MC_ITEM_FOOD_FLAG_PRESENT)) {
        cancel_item_use(c);
        return 0;
    }

    c->food = clamp_food_level(c->food);
    c->food_saturation = clamp_food_saturation(c->food, c->food_saturation);
    c->food_exhaustion = clamp_food_exhaustion(c->food_exhaustion);
    if (c->food >= PLAYER_MAX_FOOD_LEVEL && !(food->flags & MC_ITEM_FOOD_FLAG_ALWAYS_EDIBLE)) {
        cancel_item_use(c);
        return 0;
    }

    return start_food_use_cycle(c, 0, idx, slot->item_id);
}

static int complete_active_food_use(mc_conn_t *c, const mc_item_food_entry_t *food) {
    int32_t nutrition = 0;
    float saturation = 0.0f;
    int32_t new_food = 0;
    float new_saturation = 0.0f;

    if (!c || !c->player || !food) return -1;
    nutrition = food->nutrition;
    saturation = food->saturation;

    new_food = clamp_food_level(c->food + nutrition);
    new_saturation = clamp_food_saturation(new_food, c->food_saturation + saturation);
    c->food = new_food;
    c->food_saturation = new_saturation;
    c->player->food_level = new_food;
    c->player->food_saturation = new_saturation;
    c->player->food_exhaustion = c->food_exhaustion;

    if (consume_selected_item(c) != 0) {
        cancel_item_use(c);
        return -1;
    }
    if (stop_active_item_use(c) != 0) {
        cancel_item_use(c);
        return -1;
    }
    if (player_sync_food_state(c, false) != 0) {
        cancel_item_use(c);
        return -1;
    }
    return maybe_restart_food_use_cycle(c);
}

static int tick_item_use(mc_conn_t *c) {
    const mc_item_food_entry_t *food = NULL;

    if (!c || !c->is_using_item) return 0;
    if (!player_can_use_hunger_system(c) || c->dead) {
        cancel_item_use(c);
        return 0;
    }
    if (!active_food_use_matches(c, NULL, &food)) {
        cancel_item_use(c);
        return 0;
    }
    if (c->use_item_remaining_ticks > 0) c->use_item_remaining_ticks--;
    if (c->use_item_remaining_ticks > 0) return 0;
    return complete_active_food_use(c, food);
}

typedef struct {
    bool present;
    int32_t item_id;
    int32_t count;
    int32_t damage;
    size_t components_len;
} furnace_slot_sig_t;

static void furnace_slot_sig_capture(const mc_slot_t *slot, furnace_slot_sig_t *sig) {
    if (!sig) return;
    memset(sig, 0, sizeof(*sig));
    if (!slot || !slot->present || slot->count <= 0) return;
    sig->present = true;
    sig->item_id = slot->item_id;
    sig->count = slot->count;
    sig->damage = slot->damage;
    sig->components_len = slot->components_len;
}

static bool furnace_slot_sig_differs(const mc_slot_t *slot, const furnace_slot_sig_t *sig) {
    furnace_slot_sig_t now = {0};
    furnace_slot_sig_capture(slot, &now);
    return !sig || now.present != sig->present || now.item_id != sig->item_id ||
           now.count != sig->count || now.damage != sig->damage ||
           now.components_len != sig->components_len;
}

static int tick_open_furnace_container(mc_conn_t *c) {
    if (!c || !c->active_window.open || !c->active_window.container) return 0;
    mc_container_instance_t *container = c->active_window.container;
    mc_furnace_machine_t machine = mc_furnace_machine_for_container_kind(container->kind);
    if (machine == MC_FURNACE_MACHINE_NONE) return 0;

    furnace_slot_sig_t before_slots[MC_FURNACE_SLOT_COUNT];
    for (int i = 0; i < MC_FURNACE_SLOT_COUNT; i++) {
        furnace_slot_sig_capture(&container->slots[i], &before_slots[i]);
    }
    int rc = mc_furnace_tick(container, machine);
    if (rc < 0) return -1;
    bool is_burning = container->furnace_burn_time > 0;
    if (update_open_furnace_lit_state(c, is_burning) != 0) return -1;
    if (rc > 0) {
        bool slots_changed = false;
        for (int i = 0; i < MC_FURNACE_SLOT_COUNT; i++) {
            if (furnace_slot_sig_differs(&container->slots[i], &before_slots[i])) {
                slots_changed = true;
                break;
            }
        }
        if (slots_changed) {
            if (sync_active_container_window(c) != 0) return -1;
        } else if (send_furnace_container_data(c, container) != 0) {
            return -1;
        }
    }
    return 0;
}

static bool furnace_slots_can_stack(const mc_slot_t *a, const mc_slot_t *b) {
    if (!a || !b || !a->present || !b->present) return false;
    if (!mc_slot_is_same_item(a, b)) return false;
    if (a->added_component_count != b->added_component_count ||
        a->removed_component_count != b->removed_component_count ||
        a->components_len != b->components_len) {
        return false;
    }
    if (a->components_len > 0 && memcmp(a->components, b->components, a->components_len) != 0) return false;
    return true;
}

static int furnace_insert_stack(mc_slot_t *dst, mc_slot_t *src) {
    const int max_stack = 64;
    int move = 0;

    if (!dst || !src || !src->present || src->count <= 0 || src->item_id <= 0) return 0;

    if (dst->present && !furnace_slots_can_stack(dst, src)) return 0;
    if (dst->present && dst->count >= max_stack) return 0;

    if (!dst->present || dst->count <= 0) {
        move = src->count > max_stack ? max_stack : src->count;
        if (mc_slot_copy(dst, src) != 0) return -1;
        dst->count = move;
    } else {
        int room = max_stack - dst->count;
        move = src->count < room ? src->count : room;
        dst->count += move;
    }

    src->count -= move;
    if (src->count <= 0) mc_slot_clear(src);
    return move;
}

static int finish_furnace_quick_move(mc_conn_t *c, mc_container_instance_t *container) {
    if (!c || !container) return -1;

    container->state_id++;
    container->dirty = true;

    mc_world_t *world = get_world(c);
    if (!world || mc_world_mark_chunk_dirty_at(world, container->x, container->z) != 0) return -1;
    if (sync_active_container_window(c) != 0) return -1;
    if (save_active_window(c) != 0) return -1;
    return save_player_data(c);
}

static int furnace_quick_move_from_player(mc_conn_t *c, mc_container_instance_t *container, int16_t window_slot) {
    if (!c || !c->player || !container || !mc_furnace_container_kind_is_machine(container->kind)) return 0;
    if (window_slot < container->slot_count) return 0;

    mc_slot_t *src = player_window_slot(c->player, window_slot, container->slot_count);
    if (!src || !src->present || src->count <= 0) return 0;

    mc_furnace_machine_t machine = mc_furnace_machine_for_container_kind(container->kind);
    const mc_cooking_recipe_t *recipe = mc_furnace_find_recipe(machine, src->item_id);
    bool is_fuel = mc_furnace_fuel_burn_ticks(src->item_id) > 0;
    int moved = 0;

    if (recipe) {
        int rc = furnace_insert_stack(&container->slots[MC_FURNACE_INPUT_SLOT], src);
        if (rc < 0) return -1;
        moved += rc;
    }
    if (moved == 0 && is_fuel) {
        int rc = furnace_insert_stack(&container->slots[MC_FURNACE_FUEL_SLOT], src);
        if (rc < 0) return -1;
        moved += rc;
    }
    if (moved <= 0) return 0;

    c->player->inventory.state_id++;
    return finish_furnace_quick_move(c, container);
}

static int furnace_quick_move_to_player(mc_conn_t *c, mc_container_instance_t *container, int16_t window_slot) {
    if (!c || !c->player || !container || !mc_furnace_container_kind_is_machine(container->kind)) return 0;
    if (window_slot < 0 || window_slot >= MC_FURNACE_SLOT_COUNT) return 0;

    mc_slot_t *src = &container->slots[window_slot];
    int absorbed = mc_inventory_try_absorb_slot(&c->player->inventory, src);
    if (absorbed < 0) return -1;
    if (absorbed <= 0) return 0;

    return finish_furnace_quick_move(c, container);
}

static int furnace_quick_move_window_slot(mc_conn_t *c, mc_container_instance_t *container, int16_t window_slot) {
    if (!container || !mc_furnace_container_kind_is_machine(container->kind)) return 0;
    if (window_slot >= 0 && window_slot < container->slot_count) {
        return furnace_quick_move_to_player(c, container, window_slot);
    }
    return furnace_quick_move_from_player(c, container, window_slot);
}

static int drop_selected_mainhand_items(mc_conn_t *c, int32_t requested_count) {
    mc_inventory_t *inv = NULL;
    mc_slot_t dropped = {0};
    mc_slot_t *slot = NULL;
    int idx = -1;
    double drop_x = 0.0;
    double drop_y = 0.0;
    double drop_z = 0.0;
    double drop_vx = 0.0;
    double drop_vy = 0.0;
    double drop_vz = 0.0;
    int rc = 0;

    if (!c || !c->player || requested_count <= 0) return 0;
    if (c->gamemode == GAMEMODE_SPECTATOR) return 0;
    cancel_item_use(c);

    inv = conn_inventory(c);
    if (!inv) return -1;
    idx = mc_inventory_selected_slot_index(inv);
    if (idx < 0 || idx >= MC_PLAYER_SLOT_COUNT) return 0;
    slot = &inv->slots[idx];

    rc = split_slot_for_drop(slot, requested_count, &dropped);
    if (rc <= 0) {
        mc_slot_clear(&dropped);
        return rc;
    }

    inv->state_id++;
    if (sync_inventory_slot(c, (int16_t)idx) != 0) {
        mc_slot_clear(&dropped);
        return -1;
    }
    if (save_player_data(c) != 0) {
        mc_slot_clear(&dropped);
        return -1;
    }

    manual_drop_spawn_motion(c, &drop_x, &drop_y, &drop_z, &drop_vx, &drop_vy, &drop_vz);
    if (spawn_manual_drop_slot(c, drop_x, drop_y, drop_z, drop_vx, drop_vy, drop_vz, &dropped) != 0) {
        mc_slot_clear(&dropped);
        return -1;
    }
    mc_slot_clear(&dropped);
    return 1;
}

static int try_consume_selected_food(mc_conn_t *c) {
    const mc_slot_t *slot = NULL;
    const mc_item_food_entry_t *food = NULL;
    int idx = -1;

    if (!c || !c->player) return -1;
    if (c->gamemode == GAMEMODE_CREATIVE || c->gamemode == GAMEMODE_SPECTATOR) return 0;

    c->use_item_input_held = true;
    if (c->is_using_item && active_food_use_matches(c, NULL, NULL)) return 1;
    if (c->is_using_item && stop_active_item_use(c) != 0) {
        cancel_item_use(c);
        return -1;
    }

    slot = selected_mainhand_slot(c);
    idx = selected_mainhand_slot_index(c);
    if (!slot || !slot->present || slot->count <= 0) {
        cancel_item_use(c);
        return 0;
    }
    if (idx < 0 || idx >= MC_PLAYER_SLOT_COUNT) {
        cancel_item_use(c);
        return 0;
    }
    food = mc_item_food_entry(slot->item_id);
    if (!food || !(food->flags & MC_ITEM_FOOD_FLAG_PRESENT)) {
        cancel_item_use(c);
        return 0;
    }

    c->food = clamp_food_level(c->food);
    c->food_saturation = clamp_food_saturation(c->food, c->food_saturation);
    c->food_exhaustion = clamp_food_exhaustion(c->food_exhaustion);
    if (c->food >= PLAYER_MAX_FOOD_LEVEL && !(food->flags & MC_ITEM_FOOD_FLAG_ALWAYS_EDIBLE)) {
        cancel_item_use(c);
        return 0;
    }

    return start_food_use_cycle(c, 0, idx, slot->item_id) == 0 ? 1 : -1;
}

static int handle_set_creative_slot(mc_conn_t *c, const mc_frame_t *frame) {
    if (!c || !frame || !c->player) return -1;
    mc_reader_t r = {frame->payload.data, frame->payload.len, 0};
    int16_t slot_id = -1;
    if (r_i16(&r, &slot_id) != 0) return -1;

    mc_slot_t slot = {0};
    if (mc_slot_read_net(r.data, r.len, &r.pos, &slot) != 0) return -1;
    if (slot_id < 0 || slot_id >= MC_PLAYER_SLOT_COUNT) {
        mc_slot_clear(&slot);
        return 0;
    }

    mc_inventory_t *inv = &c->player->inventory;
    if (mc_slot_copy(&inv->slots[slot_id], &slot) != 0) {
        mc_slot_clear(&slot);
        return -1;
    }
    mc_slot_clear(&slot);
    inv->state_id++;
    if (slot_id == PLAYER_CRAFTING_RESULT_SLOT ||
        (slot_id >= PLAYER_CRAFTING_GRID_SLOT &&
         slot_id < PLAYER_CRAFTING_GRID_SLOT + PLAYER_CRAFTING_GRID_WIDTH * PLAYER_CRAFTING_GRID_HEIGHT)) {
        if (update_player_crafting_result(c) != 0) return -1;
        if (sync_inventory_full(c) != 0) return -1;
        return save_player_data(c);
    }
    if (debug_place_enabled()) {
        const mc_slot_t *cur = &inv->slots[slot_id];
        const char *item_name = (cur && cur->present) ? mc_minecraft_item_name(cur->item_id) : NULL;
        log_info("place debug: creative slot=%d item_id=%d item=%s count=%d", slot_id, cur->present ? cur->item_id : -1,
                 item_name ? item_name : "(empty)", cur->present ? cur->count : 0);
    }
    if (sync_inventory_slot(c, slot_id) != 0) return -1;
    return save_player_data(c);
}

static const mc_slot_t *window_slot_const(const mc_conn_t *c, int32_t window_id, int16_t window_slot) {
    if (!c || !c->player) return NULL;
    if (window_id == 0) {
        if (window_slot < 0 || window_slot >= MC_PLAYER_SLOT_COUNT) return NULL;
        return &c->player->inventory.slots[window_slot];
    }
    if (!c->active_window.open || window_id != c->active_window.window_id) return NULL;
    if (window_slot >= 0 && window_slot < c->active_window.slot_count && c->active_window.container) {
        return &c->active_window.container->slots[window_slot];
    }
    return player_window_slot_const(c->player, window_slot, c->active_window.slot_count);
}

static int snapshot_window_slot(const mc_conn_t *c, int32_t window_id, int16_t window_slot, mc_slot_t *out) {
    const mc_slot_t *slot = NULL;

    if (!out) return -1;
    slot = window_slot_const(c, window_id, window_slot);
    if (!slot || !slot->present || slot->count <= 0) return 0;
    return mc_slot_copy(out, slot) == 0 ? 1 : -1;
}

static int spawn_window_throw_drop(mc_conn_t *c, int32_t window_id, int16_t window_slot, const mc_slot_t *before) {
    const mc_slot_t *after = NULL;
    mc_slot_t dropped = {0};
    int32_t after_count = 0;
    int32_t drop_count = 0;
    double drop_x = 0.0;
    double drop_y = 0.0;
    double drop_z = 0.0;
    double drop_vx = 0.0;
    double drop_vy = 0.0;
    double drop_vz = 0.0;

    if (!c || !before || !before->present || before->count <= 0) return 0;

    after = window_slot_const(c, window_id, window_slot);
    if (after && after->present && after->count > 0) {
        if (!mc_slot_is_same_item(before, after)) return 0;
        after_count = after->count;
    }

    drop_count = before->count - after_count;
    if (drop_count <= 0) return 0;
    if (mc_slot_copy(&dropped, before) != 0) return -1;
    dropped.count = drop_count;

    manual_drop_spawn_motion(c, &drop_x, &drop_y, &drop_z, &drop_vx, &drop_vy, &drop_vz);
    if (spawn_manual_drop_slot(c, drop_x, drop_y, drop_z, drop_vx, drop_vy, drop_vz, &dropped) != 0) {
        mc_slot_clear(&dropped);
        return -1;
    }
    mc_slot_clear(&dropped);
    return 1;
}

static int handle_window_click(mc_conn_t *c, const mc_frame_t *frame) {
    if (!c || !frame || !c->player) return -1;
    mc_reader_t r = {frame->payload.data, frame->payload.len, 0};
    int32_t window_id = 0;
    int32_t state_id = 0;
    int16_t slot = 0;
    int8_t mouse_button = 0;
    int32_t container_input = 0;
    int32_t changed_count = 0;
    mc_slot_t throw_before = {0};
    bool throw_snapshot = false;
#define RETURN_WINDOW_CLICK(rc) \
    do { \
        mc_slot_clear(&throw_before); \
        return (rc); \
    } while (0)

    if (r_varint(&r, &window_id) != 0 || r_varint(&r, &state_id) != 0 || r_i16(&r, &slot) != 0) return -1;
    if (r.pos + 1 > r.len) return -1;
    mouse_button = (int8_t)r.data[r.pos++];
    if (r_varint(&r, &container_input) != 0 || r_varint(&r, &changed_count) != 0) return -1;
    (void)state_id;
    (void)mouse_button;

    bool clicked_player_crafting_result = window_id == 0 && slot == PLAYER_CRAFTING_RESULT_SLOT;
    bool clicked_active_crafting_result =
        window_id != 0 && c->active_window.open && window_id == c->active_window.window_id &&
        c->active_window.container && c->active_window.container->kind == MC_CONTAINER_KIND_CRAFTING_TABLE &&
        slot == CRAFTING_TABLE_RESULT_SLOT;
    if (container_input == MC_CONTAINER_INPUT_THROW && (clicked_player_crafting_result || clicked_active_crafting_result)) {
        if (clicked_player_crafting_result) {
            if (sync_inventory_full(c) != 0) RETURN_WINDOW_CLICK(-1);
        } else {
            if (sync_active_container_window(c) != 0) RETURN_WINDOW_CLICK(-1);
        }
        RETURN_WINDOW_CLICK(0);
    }

    if (container_input == MC_CONTAINER_INPUT_THROW && slot >= 0) {
        int snapshot_rc = snapshot_window_slot(c, window_id, slot, &throw_before);
        if (snapshot_rc < 0) {
            RETURN_WINDOW_CLICK(-1);
        }
        throw_snapshot = snapshot_rc > 0;
    }

    if (container_input == MC_CONTAINER_INPUT_PICKUP && clicked_player_crafting_result) {
        int craft_rc = take_player_crafting_result(c);
        if (craft_rc < 0) RETURN_WINDOW_CLICK(-1);
        if (craft_rc == 0 && sync_inventory_full(c) != 0) RETURN_WINDOW_CLICK(-1);
        RETURN_WINDOW_CLICK(0);
    }

    if (window_id != 0) {
        if (!c->active_window.open || window_id != c->active_window.window_id || !c->active_window.container) RETURN_WINDOW_CLICK(0);
        mc_container_instance_t *container = c->active_window.container;
        mc_world_t *world = get_world(c);
        if (container_input == MC_CONTAINER_INPUT_PICKUP && clicked_active_crafting_result) {
            int craft_rc = take_container_crafting_result(c, container);
            if (craft_rc < 0) RETURN_WINDOW_CLICK(-1);
            if (craft_rc == 0 && sync_active_container_window(c) != 0) RETURN_WINDOW_CLICK(-1);
            RETURN_WINDOW_CLICK(0);
        }
        if (container_input == MC_CONTAINER_INPUT_QUICK_MOVE && clicked_active_crafting_result) {
            int craft_rc = quick_move_container_crafting_result(c, container);
            if (craft_rc < 0) RETURN_WINDOW_CLICK(-1);
            if (craft_rc == 0 && sync_active_container_window(c) != 0) RETURN_WINDOW_CLICK(-1);
            RETURN_WINDOW_CLICK(0);
        }
        if (container_input == MC_CONTAINER_INPUT_QUICK_MOVE && mc_furnace_container_kind_is_machine(container->kind)) {
            int move_rc = furnace_quick_move_window_slot(c, container, slot);
            if (move_rc < 0) RETURN_WINDOW_CLICK(-1);
            if (move_rc == 0 && sync_active_container_window(c) != 0) RETURN_WINDOW_CLICK(-1);
            RETURN_WINDOW_CLICK(0);
        }
        for (int32_t i = 0; i < changed_count; i++) {
            int16_t location = -1;
            mc_slot_t item = {0};
            if (r_i16(&r, &location) != 0 || r_hashed_stack(&r, &item) != 0) {
                mc_slot_clear(&item);
                RETURN_WINDOW_CLICK(-1);
            }
            mc_slot_t *dst = active_container_slot(c, location);
            if (dst) {
                if (container->kind == MC_CONTAINER_KIND_CRAFTING_TABLE && location == CRAFTING_TABLE_RESULT_SLOT) {
                    mc_slot_clear(&item);
                    continue;
                }
                if (mc_slot_copy(dst, &item) != 0) {
                    mc_slot_clear(&item);
                    RETURN_WINDOW_CLICK(-1);
                }
                if (container->kind != MC_CONTAINER_KIND_CRAFTING_TABLE) container->dirty = true;
            } else {
                dst = player_window_slot(c->player, location, container->slot_count);
                if (dst) {
                    if (mc_slot_copy(dst, &item) != 0) {
                        mc_slot_clear(&item);
                        RETURN_WINDOW_CLICK(-1);
                    }
                }
            }
            mc_slot_clear(&item);
        }

        mc_slot_t cursor = {0};
        if (r_hashed_stack(&r, &cursor) != 0) {
            mc_slot_clear(&cursor);
            RETURN_WINDOW_CLICK(-1);
        }
        if (mc_slot_copy(&c->player->inventory.cursor_slot, &cursor) != 0) {
            mc_slot_clear(&cursor);
            RETURN_WINDOW_CLICK(-1);
        }
        mc_slot_clear(&cursor);

        container->state_id++;
        c->player->inventory.state_id++;
        if (container->kind == MC_CONTAINER_KIND_CRAFTING_TABLE) {
            if (update_container_crafting_result(container) != 0) RETURN_WINDOW_CLICK(-1);
        }
        if (container->kind != MC_CONTAINER_KIND_ENDER_CHEST && container->kind != MC_CONTAINER_KIND_CRAFTING_TABLE) {
            if (!world || mc_world_mark_chunk_dirty_at(world, container->x, container->z) != 0) RETURN_WINDOW_CLICK(-1);
        }
        if (container->kind == MC_CONTAINER_KIND_ENDER_CHEST) {
            c->player->ender_state_id = container->state_id;
            for (int i = 0; i < MC_CONTAINER_SLOT_COUNT; i++) {
                if (mc_slot_copy(&c->player->ender_chest[i], &container->slots[i]) != 0) RETURN_WINDOW_CLICK(-1);
            }
        }
        if (debug_place_enabled()) {
            log_info("place debug: container_click window=%d changed=%d state_id=%d", window_id, changed_count, container->state_id);
        }
        if (sync_active_container_window(c) != 0) RETURN_WINDOW_CLICK(-1);
        if (save_active_window(c) != 0) RETURN_WINDOW_CLICK(-1);
        if (save_player_data(c) != 0) RETURN_WINDOW_CLICK(-1);
        if (throw_snapshot) {
            int drop_rc = spawn_window_throw_drop(c, window_id, slot, &throw_before);
            if (drop_rc < 0) RETURN_WINDOW_CLICK(-1);
        }
        RETURN_WINDOW_CLICK(0);
    }

    mc_inventory_t *inv = &c->player->inventory;
    if (container_input == MC_CONTAINER_INPUT_QUICK_MOVE && clicked_player_crafting_result) {
        int craft_rc = quick_move_player_crafting_result(c);
        if (craft_rc < 0) RETURN_WINDOW_CLICK(-1);
        if (craft_rc == 0 && sync_inventory_full(c) != 0) RETURN_WINDOW_CLICK(-1);
        RETURN_WINDOW_CLICK(0);
    }
    for (int32_t i = 0; i < changed_count; i++) {
        int16_t location = -1;
        mc_slot_t item = {0};
        if (r_i16(&r, &location) != 0 || r_hashed_stack(&r, &item) != 0) {
            mc_slot_clear(&item);
            RETURN_WINDOW_CLICK(-1);
        }
        if (location >= 0 && location < MC_PLAYER_SLOT_COUNT) {
            if (location == PLAYER_CRAFTING_RESULT_SLOT) {
                mc_slot_clear(&item);
                continue;
            }
            if (mc_slot_copy(&inv->slots[location], &item) != 0) {
                mc_slot_clear(&item);
                RETURN_WINDOW_CLICK(-1);
            }
        }
        mc_slot_clear(&item);
    }

    mc_slot_t cursor = {0};
    if (r_hashed_stack(&r, &cursor) != 0) {
        mc_slot_clear(&cursor);
        RETURN_WINDOW_CLICK(-1);
    }
    if (mc_slot_copy(&inv->cursor_slot, &cursor) != 0) {
        mc_slot_clear(&cursor);
        RETURN_WINDOW_CLICK(-1);
    }
    mc_slot_clear(&cursor);

    inv->state_id++;
    if (update_player_crafting_result(c) != 0) RETURN_WINDOW_CLICK(-1);
    if (debug_place_enabled()) {
        log_info("place debug: window_click changed=%d state_id=%d", changed_count, inv->state_id);
    }
    if (sync_inventory_full(c) != 0) RETURN_WINDOW_CLICK(-1);
    if (save_player_data(c) != 0) RETURN_WINDOW_CLICK(-1);
    if (throw_snapshot) {
        int drop_rc = spawn_window_throw_drop(c, window_id, slot, &throw_before);
        if (drop_rc < 0) RETURN_WINDOW_CLICK(-1);
    }
    RETURN_WINDOW_CLICK(0);
#undef RETURN_WINDOW_CLICK
}

static int handle_command(mc_conn_t *c, char *cmdline) {
    if (!c || !cmdline) return 0;
    while (*cmdline && isspace((unsigned char)*cmdline)) cmdline++;
    if (*cmdline == '/') cmdline++;
    char *save = NULL;
    char *cmd = strtok_r(cmdline, " ", &save);
    if (!cmd) return 0;

    char cmd_lower[32];
    size_t cmd_len = strlen(cmd);
    if (cmd_len >= sizeof(cmd_lower)) cmd_len = sizeof(cmd_lower) - 1;
    for (size_t i = 0; i < cmd_len; i++) cmd_lower[i] = (char)tolower((unsigned char)cmd[i]);
    cmd_lower[cmd_len] = '\0';

    if (strcmp(cmd_lower, "gamemode") == 0) {
        char *mode = strtok_r(NULL, " ", &save);
        int gm = parse_gamemode(mode);
        if (gm >= 0 && gm != c->gamemode) {
            c->gamemode = gm;
            if (c->player) c->player->gamemode = gm;
            if (send_game_mode_event(c, gm) != 0) return -1;
            if (send_player_abilities(c) != 0) return -1;
            (void)save_player_data(c);
        }
        return 0;
    }

    if (strcmp(cmd_lower, "tp") == 0 || strcmp(cmd_lower, "teleport") == 0) {
        char *a = strtok_r(NULL, " ", &save);
        char *b = strtok_r(NULL, " ", &save);
        char *cstr = strtok_r(NULL, " ", &save);
        char *d = strtok_r(NULL, " ", &save);
        if (!a) return 0;

        if (b && cstr && !d) {
            double x = 0, y = 0, z = 0;
            if (parse_double(a, &x) && parse_double(b, &y) && parse_double(cstr, &z)) {
                return send_sync_position(c, x, y, z, c->yaw, c->pitch);
            }
        }

        if (b && cstr && d) {
            double x = 0, y = 0, z = 0;
            bool target_release = false;
            mc_conn_t *target = resolve_player(c, a, &target_release);
            if (!target) return 0;
            if (parse_double(b, &x) && parse_double(cstr, &y) && parse_double(d, &z)) {
                int rc = send_sync_position(target, x, y, z, target->yaw, target->pitch);
                if (target_release) net_server_release_conn(target);
                return rc;
            }
            if (target_release) net_server_release_conn(target);
        }

        if (b && !cstr) {
            bool target_release = false;
            bool dest_release = false;
            mc_conn_t *target = resolve_player(c, a, &target_release);
            mc_conn_t *dest = resolve_player(c, b, &dest_release);
            if (!target || !dest || !dest->has_pos) {
                if (target_release) net_server_release_conn(target);
                if (dest_release) net_server_release_conn(dest);
                return 0;
            }
            int rc = send_sync_position(target, dest->x, dest->y, dest->z, dest->yaw, dest->pitch);
            if (target_release) net_server_release_conn(target);
            if (dest_release) net_server_release_conn(dest);
            return rc;
        }
        return 0;
    }

    if (strcmp(cmd_lower, "setblock") == 0) {
        char *xs = strtok_r(NULL, " ", &save);
        char *ys = strtok_r(NULL, " ", &save);
        char *zs = strtok_r(NULL, " ", &save);
        char *blk = strtok_r(NULL, " ", &save);
        int32_t x = 0, y = 0, z = 0;
        if (!xs || !ys || !zs || !blk) return 0;
        if (!parse_i32(xs, &x) || !parse_i32(ys, &y) || !parse_i32(zs, &z)) return 0;

        char name[128];
        if (strlen(blk) >= sizeof(name) - 1) return 0;
        strcpy(name, blk);
        for (size_t i = 0; name[i]; i++) {
            name[i] = (char)tolower((unsigned char)name[i]);
        }
        char full[160];
        const char *norm = name;
        if (!strchr(name, ':')) {
            snprintf(full, sizeof(full), "minecraft:%s", name);
            norm = full;
        }

        mc_world_t *world = get_world(c);
        const mc_world_ids_t *ids = mc_world_ids(world);
        if (!world || !ids) return 0;

        int32_t sid = -1;
        if (strcmp(norm, "minecraft:air") == 0) sid = ids->air;
        else if (strcmp(norm, "minecraft:stone") == 0) sid = ids->stone;
        else if (strcmp(norm, "minecraft:water") == 0) sid = ids->water_level[0];
        else if (strcmp(norm, "minecraft:lava") == 0) sid = ids->lava_level[0];
        else if (strcmp(norm, "minecraft:fire") == 0) sid = ids->fire_age[0];
        else if (strcmp(norm, "minecraft:redstone_wire") == 0) sid = ids->wire_power[0];
        else if (strcmp(norm, "minecraft:redstone_block") == 0) sid = ids->redstone_block;
        else if (strcmp(norm, "minecraft:redstone_lamp") == 0) sid = ids->lamp_lit[0];

        if (sid < 0) return 0;
        (void)mc_world_set_block(world, x, y, z, sid);
        return 0;
    }

    if (strcmp(cmd_lower, "difficulty") == 0 || strcmp(cmd_lower, "difficulte") == 0) {
        char *sub = strtok_r(NULL, " ", &save);
        mc_difficulty_t difficulty = current_difficulty(c);

        if (!sub || strcmp(sub, "get") == 0 || strcmp(sub, "show") == 0) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Current difficulty: %s", mc_difficulty_name(difficulty));
            return send_system_message(c, msg);
        }

        const char *value = sub;
        if (strcmp(sub, "set") == 0) {
            value = strtok_r(NULL, " ", &save);
            if (!value) {
                return send_system_message(c, "Usage: /difficulty set <peaceful|easy|normal|hard>");
            }
        }

        if (!parse_difficulty_text(value, &difficulty)) {
            return send_system_message(c, "Usage: /difficulty <peaceful|easy|normal|hard>");
        }

        net_server_set_difficulty(c->server, difficulty);
        log_info("difficulty set to %s by %s", mc_difficulty_name(difficulty), c->username[0] ? c->username : "(unknown)");
        if (net_server_broadcast_difficulty(c->server) != 0) return -1;

        char msg[96];
        snprintf(msg, sizeof(msg), "Difficulty set to %s", mc_difficulty_name(difficulty));
        return send_system_message(c, msg);
    }

    if (strcmp(cmd_lower, "food") == 0) {
        char *sub = strtok_r(NULL, " ", &save);
        char *arg = strtok_r(NULL, " ", &save);
        if (!sub || !arg || !c->player) return 0;

        if (strcmp(sub, "set") == 0) {
            int32_t value = 0;
            if (!parse_i32(arg, &value)) return 0;
            c->food = clamp_food_level(value);
            if (c->food_saturation > (float)c->food) c->food_saturation = (float)c->food;
            player_reset_food_debug_runtime(c);
            return player_sync_food_state(c, true);
        }
        if (strcmp(sub, "add") == 0) {
            int32_t delta = 0;
            if (!parse_i32(arg, &delta)) return 0;
            c->food = clamp_food_level(c->food + delta);
            if (c->food_saturation > (float)c->food) c->food_saturation = (float)c->food;
            player_reset_food_debug_runtime(c);
            return player_sync_food_state(c, true);
        }
        if (strcmp(sub, "sat") == 0) {
            float value = 0.0f;
            if (!parse_f32_text(arg, &value)) return 0;
            c->food_saturation = clamp_food_saturation(c->food, value);
            return player_sync_food_state(c, true);
        }
        if (strcmp(sub, "exhaust") == 0) {
            float value = 0.0f;
            if (!parse_f32_text(arg, &value)) return 0;
            c->food_exhaustion = clamp_food_exhaustion(value);
            return player_apply_exhaustion_thresholds(c, true);
        }
        return 0;
    }

    if (strcmp(cmd_lower, "health") == 0 || strcmp(cmd_lower, "vie") == 0) {
        char *sub = strtok_r(NULL, " ", &save);
        char *arg = strtok_r(NULL, " ", &save);
        if (!sub || !c->player) return 0;

        if (strcmp(sub, "kill") == 0) {
            return player_set_health_debug(c, 0.0f, "Killed by debug command");
        }
        if (!arg) return 0;
        if (strcmp(sub, "set") == 0) {
            float value = 0.0f;
            if (!parse_f32_text(arg, &value)) return 0;
            return player_set_health_debug(c, value, "Killed by debug command");
        }
        if (strcmp(sub, "add") == 0) {
            float delta = 0.0f;
            if (!parse_f32_text(arg, &delta)) return 0;
            return player_set_health_debug(c, c->health + delta, "Killed by debug command");
        }
        if (strcmp(sub, "damage") == 0 || strcmp(sub, "dmg") == 0) {
            float amount = 0.0f;
            if (!parse_f32_text(arg, &amount) || amount < 0.0f) return 0;
            return player_set_health_debug(c, c->health - amount, "Killed by debug command");
        }
        if (strcmp(sub, "heal") == 0) {
            float amount = 0.0f;
            if (!parse_f32_text(arg, &amount) || amount < 0.0f) return 0;
            return player_set_health_debug(c, c->health + amount, "Killed by debug command");
        }
        return 0;
    }

    if (strcmp(cmd_lower, "kill") == 0) {
        if (!c->player) return 0;
        return player_set_health_debug(c, 0.0f, "Killed by debug command");
    }

    return 0;
}

typedef struct {
    bool loaded;
    char *path;
    uint8_t *heightmaps;
    size_t heightmaps_len;
    uint8_t *light;
    size_t light_len;
    uint8_t *fullbright_light;
    size_t fullbright_light_len;
    int32_t biome_id;
} mc_chunk_template_t;

static mc_chunk_template_t g_chunk_tpl = {0};

static void chunk_template_free(mc_chunk_template_t *t) {
    if (!t) return;
    free(t->path);
    free(t->heightmaps);
    free(t->light);
    free(t->fullbright_light);
    memset(t, 0, sizeof(*t));
}

static int buf_w_u8(mc_buf_t *b, uint8_t v) {
    return buf_write(b, &v, 1);
}

static int buf_w_u16_be(mc_buf_t *b, uint16_t v) {
    uint8_t tmp[2];
    tmp[0] = (uint8_t)((v >> 8) & 0xFF);
    tmp[1] = (uint8_t)(v & 0xFF);
    return buf_write(b, tmp, sizeof(tmp));
}

static int buf_w_i32_be(mc_buf_t *b, int32_t v) {
    uint8_t tmp[4];
    tmp[0] = (uint8_t)((v >> 24) & 0xFF);
    tmp[1] = (uint8_t)((v >> 16) & 0xFF);
    tmp[2] = (uint8_t)((v >> 8) & 0xFF);
    tmp[3] = (uint8_t)(v & 0xFF);
    return buf_write(b, tmp, sizeof(tmp));
}

static int buf_w_u64_be(mc_buf_t *b, uint64_t v) {
    uint8_t tmp[8];
    tmp[0] = (uint8_t)((v >> 56) & 0xFF);
    tmp[1] = (uint8_t)((v >> 48) & 0xFF);
    tmp[2] = (uint8_t)((v >> 40) & 0xFF);
    tmp[3] = (uint8_t)((v >> 32) & 0xFF);
    tmp[4] = (uint8_t)((v >> 24) & 0xFF);
    tmp[5] = (uint8_t)((v >> 16) & 0xFF);
    tmp[6] = (uint8_t)((v >> 8) & 0xFF);
    tmp[7] = (uint8_t)(v & 0xFF);
    return buf_write(b, tmp, sizeof(tmp));
}

static int buf_w_varint(mc_buf_t *b, int32_t v) {
    uint8_t tmp[MC_VARINT_MAX_BYTES];
    size_t n = 0;
    if (varint_write(tmp, sizeof(tmp), v, &n) != 0) return -1;
    return buf_write(b, tmp, n);
}

static int buf_w_i64_be(mc_buf_t *b, int64_t v) {
    return buf_w_u64_be(b, (uint64_t)v);
}

static int popcount_u64(uint64_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(v);
#else
    int c = 0;
    while (v) {
        c += (int)(v & 1u);
        v >>= 1;
    }
    return c;
#endif
}

static int build_motion_blocking_heightmaps(mc_world_t *world, const mc_chunk_t *chunk, uint8_t **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!world || !chunk || !out || !out_len) return -1;

    const mc_world_ids_t *ids = mc_world_ids(world);
    if (!ids) return -1;

    static int32_t cave_air_id = INT32_MIN;
    static int32_t void_air_id = INT32_MIN;
    if (cave_air_id == INT32_MIN) cave_air_id = mc_block_state_id("minecraft:cave_air", ids->air);
    if (void_air_id == INT32_MIN) void_air_id = mc_block_state_id("minecraft:void_air", ids->air);

    uint32_t vals[256];
    const int32_t top_y = MC_WORLD_MIN_Y + MC_WORLD_HEIGHT - 1;

    for (int lz = 0; lz < 16; lz++) {
        for (int lx = 0; lx < 16; lx++) {
            int32_t highest = MC_WORLD_MIN_Y - 1;
            for (int32_t y = top_y; y >= MC_WORLD_MIN_Y; y--) {
                int32_t sid = (int32_t)mc_chunk_get_block(chunk, lx, y, lz);
                if (sid == ids->air || sid == cave_air_id || sid == void_air_id) continue;
                highest = y;
                break;
            }
            uint32_t v = 0;
            if (highest >= MC_WORLD_MIN_Y) {
                v = (uint32_t)((highest - MC_WORLD_MIN_Y) + 1);
            }
            vals[(lz * 16) + lx] = v;
        }
    }

    int64_t *longs = NULL;
    int32_t long_count = 0;
    if (mc_packed_pack_compact_u32(vals, 256, 9, &longs, &long_count) != 0 || long_count != 36) {
        free(longs);
        return -1;
    }

    mc_buf_t b;
    if (buf_init(&b, 512) != 0) {
        free(longs);
        return -1;
    }

    /* 26.1 network format uses a StreamCodec map, not unnamed NBT:
     * VarInt map_size, then for each entry:
     *   VarInt heightmap_type_id,
     *   VarInt long_count,
     *   long[long_count] big-endian.
     * We only emit MOTION_BLOCKING (id=4). */
    if (buf_w_varint(&b, 1) != 0 ||
        buf_w_varint(&b, 4) != 0 ||
        buf_w_varint(&b, long_count) != 0) {
        buf_free(&b);
        free(longs);
        return -1;
    }
    for (int32_t i = 0; i < long_count; i++) {
        if (buf_w_i64_be(&b, longs[i]) != 0) {
            buf_free(&b);
            free(longs);
            return -1;
        }
    }

    uint8_t *blob = (uint8_t *)malloc(b.len ? b.len : 1);
    if (!blob) {
        buf_free(&b);
        free(longs);
        return -1;
    }
    memcpy(blob, b.data, b.len);
    *out = blob;
    *out_len = b.len;
    buf_free(&b);
    free(longs);
    return 0;
}

static int build_fullbright_light_blob(uint8_t **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!out || !out_len) return -1;

    const int light_bits = MC_WORLD_SECTION_COUNT + 2;
    uint64_t mask = (light_bits >= 64) ? UINT64_MAX : ((1ULL << light_bits) - 1ULL);
    int sky_arrays = popcount_u64(mask);

    mc_buf_t b;
    if (buf_init(&b, 1024 + (size_t)sky_arrays * 2100) != 0) return -1;

    /* skyLightMask i64[] */
    if (buf_w_varint(&b, 1) != 0 || buf_w_u64_be(&b, mask) != 0) goto fail;
    /* blockLightMask i64[] */
    if (buf_w_varint(&b, 1) != 0 || buf_w_u64_be(&b, 0) != 0) goto fail;
    /* emptySkyLightMask i64[] */
    if (buf_w_varint(&b, 1) != 0 || buf_w_u64_be(&b, 0) != 0) goto fail;
    /* emptyBlockLightMask i64[] */
    if (buf_w_varint(&b, 1) != 0 || buf_w_u64_be(&b, mask) != 0) goto fail;

    /* skyLight arrays */
    if (buf_w_varint(&b, sky_arrays) != 0) goto fail;
    uint8_t full[2048];
    memset(full, 0xFF, sizeof(full));
    for (int i = 0; i < sky_arrays; i++) {
        if (buf_w_varint(&b, (int32_t)sizeof(full)) != 0) goto fail;
        if (buf_write(&b, full, sizeof(full)) != 0) goto fail;
    }

    /* blockLight arrays */
    if (buf_w_varint(&b, 0) != 0) goto fail;

    uint8_t *blob = (uint8_t *)malloc(b.len ? b.len : 1);
    if (!blob) goto fail;
    memcpy(blob, b.data, b.len);
    *out = blob;
    *out_len = b.len;
    buf_free(&b);
    return 0;

fail:
    buf_free(&b);
    return -1;
}

static int skip_paletted_container(const uint8_t *data, size_t len, size_t *pos, int entry_count, int local_max_bits) {
    if (!data || !pos) return -1;
    if (*pos >= len) return -1;
    uint8_t bits = data[(*pos)++];

    if (bits == 0) {
        int32_t single = 0;
        size_t n = 0;
        if (varint_read(data + *pos, len - *pos, &single, &n) != 0) return -1;
        *pos += n;
        return 0;
    }

    if (bits <= local_max_bits) {
        int32_t pal_len = 0;
        size_t n = 0;
        if (varint_read(data + *pos, len - *pos, &pal_len, &n) != 0) return -1;
        *pos += n;
        if (pal_len < 0) return -1;
        for (int32_t i = 0; i < pal_len; i++) {
            int32_t tmp = 0;
            if (varint_read(data + *pos, len - *pos, &tmp, &n) != 0) return -1;
            *pos += n;
        }
    }

    int word_count = paletted_word_count_for_network(entry_count, bits);
    if (word_count < 0) return -1;
    size_t bytes = (size_t)word_count * 8u;
    if (*pos + bytes > len) return -1;
    *pos += bytes;
    return 0;
}

static int skip_heightmaps_stream_codec(const uint8_t *data, size_t len, size_t *pos) {
    if (!data || !pos) return -1;
    int32_t map_count = 0;
    size_t n = 0;
    if (varint_read(data + *pos, len - *pos, &map_count, &n) != 0) return -1;
    *pos += n;
    if (map_count < 0) return -1;

    for (int32_t i = 0; i < map_count; i++) {
        int32_t hm_type = 0;
        int32_t long_count = 0;
        if (varint_read(data + *pos, len - *pos, &hm_type, &n) != 0) return -1;
        *pos += n;
        if (varint_read(data + *pos, len - *pos, &long_count, &n) != 0) return -1;
        *pos += n;
        if (hm_type < 0 || long_count < 0) return -1;
        size_t bytes = (size_t)long_count * 8;
        if (*pos + bytes > len) return -1;
        *pos += bytes;
    }
    return 0;
}

static int parse_template_biome_id(const uint8_t *chunkdata, size_t chunkdata_len, int32_t *out_biome_id) {
    if (!chunkdata || !out_biome_id) return -1;
    size_t pos = 0;
    if (chunkdata_len < 4) return -1;
    pos += 4; /* nonEmptyBlockCount + fluidCount */
    if (skip_paletted_container(chunkdata, chunkdata_len, &pos, 4096, 8) != 0) return -1; /* blocks */
    if (pos >= chunkdata_len) return -1;
    uint8_t bits = chunkdata[pos++];
    if (bits > 3) return -1;
    if (bits == 0) {
        int32_t single = 0;
        size_t n = 0;
        if (varint_read(chunkdata + pos, chunkdata_len - pos, &single, &n) != 0) return -1;
        *out_biome_id = single;
        return 0;
    }
    int32_t pal_len = 0;
    size_t n = 0;
    if (varint_read(chunkdata + pos, chunkdata_len - pos, &pal_len, &n) != 0) return -1;
    pos += n;
    if (pal_len <= 0) return -1;
    int32_t first = -1;
    for (int32_t i = 0; i < pal_len; i++) {
        int32_t v = 0;
        if (varint_read(chunkdata + pos, chunkdata_len - pos, &v, &n) != 0) return -1;
        pos += n;
        if (i == 0) first = v;
    }
    *out_biome_id = first;
    return 0;
}

int proto_play_validate_chunkdata_for_test(const uint8_t *data, size_t len) {
    if (!data) return -1;
    size_t pos = 0;
    for (int sec = 0; sec < MC_WORLD_SECTION_COUNT; sec++) {
        if (len - pos < 4) return -1;
        pos += 4; /* nonEmptyBlockCount + fluidCount */
        if (skip_paletted_container(data, len, &pos, 4096, 8) != 0) return -1;
        if (skip_paletted_container(data, len, &pos, 64, 3) != 0) return -1;
    }
    return pos == len ? 0 : -1;
}

static int pack_simple_bitstorage_u32(const uint32_t *values, size_t value_count, int bits,
                                      uint64_t **out_words, int32_t *out_word_count) {
    size_t values_per_long;
    size_t word_count;
    uint64_t *words;
    uint64_t mask;

    if (out_words) *out_words = NULL;
    if (out_word_count) *out_word_count = 0;
    if (!values || !out_words || !out_word_count) return -1;
    if (bits <= 0 || bits > 32) return -1;

    /* PalettedContainer network encoding uses the 1.16+ SimpleBitStorage
     * layout for chunk sections: values never straddle two uint64_t words.
     * Each word holds exactly floor(64 / bits) entries, and any remaining
     * high bits in the word stay zero. This differs from our internal compact
     * storage and from heightmaps, which are allowed to use a contiguous
     * bitstream across word boundaries. */
    values_per_long = 64u / (size_t)bits;
    if (values_per_long == 0) return -1;
    word_count = (value_count + values_per_long - 1u) / values_per_long;
    if (word_count > (size_t)INT32_MAX) return -1;

    words = (uint64_t *)calloc(word_count ? word_count : 1u, sizeof(*words));
    if (!words) return -1;

    mask = (1ULL << bits) - 1ULL;
    for (size_t word_index = 0; word_index < word_count; word_index++) {
        uint64_t packed_word = 0;
        size_t base_index = word_index * values_per_long;
        size_t entries_in_word = value_count - base_index;
        if (entries_in_word > values_per_long) entries_in_word = values_per_long;

        for (size_t entry = 0; entry < entries_in_word; entry++) {
            size_t shift = entry * (size_t)bits;
            packed_word |= (((uint64_t)values[base_index + entry]) & mask) << shift;
        }

        words[word_index] = packed_word;
    }

    *out_words = words;
    *out_word_count = (int32_t)word_count;
    return 0;
}

static int load_chunk_template(mc_conn_t *c) {
    if (!c || !c->cfg) return -1;
    const char *path = c->cfg->chunk_blob_path;
    if (!path) return -1;
    if (g_chunk_tpl.loaded && g_chunk_tpl.path && strcmp(g_chunk_tpl.path, path) == 0) return 0;

    chunk_template_free(&g_chunk_tpl);

    uint8_t *blob = NULL;
    size_t blob_len = 0;
    if (read_file(path, &blob, &blob_len) != 0) {
        log_error("failed to read chunk template: %s", path);
        return -1;
    }
    if (blob_len < 8) {
        free(blob);
        return -1;
    }

    size_t pos = 8;
    size_t hm_pos = pos;
    if (skip_heightmaps_stream_codec(blob, blob_len, &pos) != 0) {
        free(blob);
        return -1;
    }
    size_t hm_bytes = pos - hm_pos;
    g_chunk_tpl.heightmaps = (uint8_t *)malloc(hm_bytes);
    if (!g_chunk_tpl.heightmaps) {
        free(blob);
        return -1;
    }
    memcpy(g_chunk_tpl.heightmaps, blob + hm_pos, hm_bytes);
    g_chunk_tpl.heightmaps_len = hm_bytes;

    int32_t chunkdata_len = 0;
    size_t n = 0;
    if (varint_read(blob + pos, blob_len - pos, &chunkdata_len, &n) != 0) {
        free(blob);
        chunk_template_free(&g_chunk_tpl);
        return -1;
    }
    pos += n;
    if (chunkdata_len < 0 || (size_t)chunkdata_len > blob_len - pos) {
        free(blob);
        chunk_template_free(&g_chunk_tpl);
        return -1;
    }
    const uint8_t *chunkdata = blob + pos;
    pos += (size_t)chunkdata_len;

    int32_t be_count = 0;
    if (varint_read(blob + pos, blob_len - pos, &be_count, &n) != 0) {
        free(blob);
        chunk_template_free(&g_chunk_tpl);
        return -1;
    }
    pos += n;
    if (be_count != 0) {
        log_error("chunk template has block entities count=%d (expected 0)", be_count);
        free(blob);
        chunk_template_free(&g_chunk_tpl);
        return -1;
    }

    if (pos > blob_len) {
        free(blob);
        chunk_template_free(&g_chunk_tpl);
        return -1;
    }
    g_chunk_tpl.light_len = blob_len - pos;
    g_chunk_tpl.light = (uint8_t *)malloc(g_chunk_tpl.light_len);
    if (!g_chunk_tpl.light) {
        free(blob);
        chunk_template_free(&g_chunk_tpl);
        return -1;
    }
    memcpy(g_chunk_tpl.light, blob + pos, g_chunk_tpl.light_len);

    g_chunk_tpl.biome_id = 21;
    bool biome_ok = (parse_template_biome_id(chunkdata, (size_t)chunkdata_len, &g_chunk_tpl.biome_id) == 0);
    if (!biome_ok) g_chunk_tpl.biome_id = 21;
    log_info("chunk template loaded: biome_id=%d (%s)", g_chunk_tpl.biome_id, biome_ok ? "parsed" : "fallback");
    if (debug_players_enabled() || debug_place_enabled()) {
        log_info("container block entity ids: chest=%d trapped_chest=%d ender_chest=%d",
                 mc_minecraft_block_entity_type_id("minecraft:chest"),
                 mc_minecraft_block_entity_type_id("minecraft:trapped_chest"),
                 mc_minecraft_block_entity_type_id("minecraft:ender_chest"));
    }

    if (build_fullbright_light_blob(&g_chunk_tpl.fullbright_light, &g_chunk_tpl.fullbright_light_len) != 0) {
        log_error("failed to build fullbright light blob; falling back to template light");
    }

    g_chunk_tpl.path = strdup(path);
    if (!g_chunk_tpl.path) {
        free(blob);
        chunk_template_free(&g_chunk_tpl);
        return -1;
    }
    g_chunk_tpl.loaded = true;
    free(blob);
    return 0;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t aa = *(const uint32_t *)a;
    uint32_t bb = *(const uint32_t *)b;
    return (aa > bb) - (aa < bb);
}

static int palette_index_of_u32(const uint32_t *palette, int pal_len, uint32_t v) {
    int lo = 0;
    int hi = pal_len - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint32_t mv = palette[mid];
        if (mv == v) return mid;
        if (mv < v) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

static int paletted_word_count_for_network(int entry_count, int bits) {
    int values_per_long;
    if (bits <= 0) return 0;
    values_per_long = 64 / bits;
    if (values_per_long <= 0) return -1;
    return (entry_count + values_per_long - 1) / values_per_long;
}

static int block_palette_bits_for_size(int pal_len) {
    if (pal_len <= 1) return 0;
    if (pal_len <= 16) return 4;
    if (pal_len <= 32) return 5;
    if (pal_len <= 64) return 6;
    if (pal_len <= 128) return 7;
    if (pal_len <= 256) return 8;
    return 15;
}

static int biome_palette_bits_for_size(int pal_len) {
    if (pal_len <= 1) return 0;
    if (pal_len <= 2) return 1;
    if (pal_len <= 4) return 2;
    if (pal_len <= 8) return 3;
    return 6;
}

static int encode_paletted_container_network(mc_buf_t *out, const uint32_t *values, int entry_count, int local_max_bits) {
    uint32_t palette_ids[4096];
    int pal_len = 0;

    if (!out || !values || entry_count <= 0 || entry_count > (int)(sizeof(palette_ids) / sizeof(palette_ids[0]))) return -1;

    for (int i = 0; i < entry_count; i++) {
        palette_ids[i] = values[i];
    }
    qsort(palette_ids, (size_t)entry_count, sizeof(palette_ids[0]), cmp_u32);
    for (int i = 0; i < entry_count; i++) {
        if (i == 0 || palette_ids[i] != palette_ids[i - 1]) palette_ids[pal_len++] = palette_ids[i];
    }

    int bits = local_max_bits == 8 ? block_palette_bits_for_size(pal_len) : biome_palette_bits_for_size(pal_len);
    if (buf_w_u8(out, (uint8_t)bits) != 0) return -1;

    if (bits == 0) {
        return buf_w_varint(out, (int32_t)palette_ids[0]);
    }

    if (bits <= local_max_bits) {
        if (buf_w_varint(out, pal_len) != 0) return -1;
        for (int i = 0; i < pal_len; i++) {
            if (buf_w_varint(out, (int32_t)palette_ids[i]) != 0) return -1;
        }
    }

    uint32_t packed_vals[4096];
    for (int i = 0; i < entry_count; i++) {
        if (bits <= local_max_bits) {
            int idx = palette_index_of_u32(palette_ids, pal_len, values[i]);
            if (idx < 0) return -1;
            packed_vals[i] = (uint32_t)idx;
        } else {
            packed_vals[i] = values[i];
        }
    }

    uint64_t *words = NULL;
    int32_t word_count = 0;
    if (pack_simple_bitstorage_u32(packed_vals, (size_t)entry_count, bits, &words, &word_count) != 0) return -1;
    for (int32_t i = 0; i < word_count; i++) {
        if (buf_w_u64_be(out, words[i]) != 0) {
            free(words);
            return -1;
        }
    }
    free(words);
    return 0;
}

int proto_play_encode_chunkdata_for_test(mc_world_t *world, const mc_chunk_t *chunk, mc_buf_t *out) {
    if (!world || !chunk || !out) return -1;
    const mc_world_ids_t *ids = mc_world_ids(world);
    if (!ids) return -1;

    for (int sec = 0; sec < MC_WORLD_SECTION_COUNT; sec++) {
        const mc_paletted_container_t *section = &chunk->sections[sec];
        uint32_t states[4096];
        for (int i = 0; i < 4096; i++) {
            int ly = i >> 8;
            int rem = i & 255;
            int lz = rem >> 4;
            int lx = rem & 15;
            uint32_t raw_state = mc_paletted_container_get_block(section, lx, ly, lz);
            int32_t normalized_state = mc_world_normalize_container_state_id((int32_t)raw_state);
            if (normalized_state < 0) return -1;
            states[i] = (uint32_t)normalized_state;
        }

        int non_air = 0;
        for (int i = 0; i < 4096; i++) {
            if ((int32_t)states[i] != ids->air) non_air++;
        }
        if (buf_w_u16_be(out, (uint16_t)non_air) != 0) return -1;
        if (buf_w_u16_be(out, 0) != 0) return -1; /* fluidCount */
        if (encode_paletted_container_network(out, states, 4096, 8) != 0) return -1;

        /* Biomes: one 4x4x4 container per section. */
        uint32_t biomes[64];
        if (g_chunk_tpl.biome_id < 0) return -1;
        for (int i = 0; i < 64; i++) biomes[i] = (uint32_t)g_chunk_tpl.biome_id;
        if (encode_paletted_container_network(out, biomes, 64, 3) != 0) return -1;
    }

    return 0;
}

static uint64_t hash_u64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static int64_t chunk_key(int32_t cx, int32_t cz) {
    return ((int64_t)cx << 32) | (uint32_t)cz;
}

static int32_t key_cx(int64_t key) {
    return (int32_t)(key >> 32);
}

static int32_t key_cz(int64_t key) {
    return (int32_t)(uint32_t)key;
}

static size_t next_pow2_size(size_t v) {
    size_t x = 1;
    while (x < v) x <<= 1;
    return x;
}

static int sent_chunks_rehash(mc_conn_t *c, size_t new_cap) {
    if (!c) return -1;
    int64_t *new_keys = (int64_t *)calloc(new_cap, sizeof(*new_keys));
    uint8_t *new_states = (uint8_t *)calloc(new_cap, 1);
    if (!new_keys || !new_states) {
        free(new_keys);
        free(new_states);
        return -1;
    }

    if (c->sent_chunks.keys && c->sent_chunks.states) {
        size_t old_cap = c->sent_chunks.cap;
        for (size_t i = 0; i < old_cap; i++) {
            if (c->sent_chunks.states[i] != 1) continue;
            int64_t key = c->sent_chunks.keys[i];
            size_t mask = new_cap - 1;
            size_t idx = (size_t)hash_u64((uint64_t)key) & mask;
            while (new_states[idx] == 1) idx = (idx + 1) & mask;
            new_states[idx] = 1;
            new_keys[idx] = key;
        }
    }

    free(c->sent_chunks.keys);
    free(c->sent_chunks.states);
    c->sent_chunks.keys = new_keys;
    c->sent_chunks.states = new_states;
    c->sent_chunks.cap = new_cap;
    c->sent_chunks.tombs = 0;
    return 0;
}

static int sent_chunks_init(mc_conn_t *c, size_t cap_hint) {
    if (!c) return -1;
    if (c->sent_chunks.cap != 0) return 0;
    size_t cap = next_pow2_size(cap_hint ? cap_hint : 1024);
    if (cap < 1024) cap = 1024;
    if (sent_chunks_rehash(c, cap) != 0) return -1;
    c->sent_chunks.len = 0;
    return 0;
}

static void sent_chunks_clear(mc_conn_t *c) {
    if (!c || !c->sent_chunks.states) return;
    memset(c->sent_chunks.states, 0, c->sent_chunks.cap);
    c->sent_chunks.len = 0;
    c->sent_chunks.tombs = 0;
}

static int sent_chunks_maybe_grow(mc_conn_t *c) {
    if (!c || c->sent_chunks.cap == 0) return -1;
    size_t used = c->sent_chunks.len + c->sent_chunks.tombs;
    if (used * 10 < c->sent_chunks.cap * 7) return 0;
    size_t new_cap = c->sent_chunks.cap * 2;
    if (new_cap < c->sent_chunks.cap) return -1;
    return sent_chunks_rehash(c, new_cap);
}

static bool sent_chunks_contains(const mc_conn_t *c, int64_t key) {
    if (!c || !c->sent_chunks.cap || !c->sent_chunks.states) return false;
    size_t mask = c->sent_chunks.cap - 1;
    size_t idx = (size_t)hash_u64((uint64_t)key) & mask;
    for (;;) {
        uint8_t st = c->sent_chunks.states[idx];
        if (st == 0) return false;
        if (st == 1 && c->sent_chunks.keys[idx] == key) return true;
        idx = (idx + 1) & mask;
    }
}

static int sent_chunks_insert(mc_conn_t *c, int64_t key) {
    if (!c) return -1;
    if (sent_chunks_maybe_grow(c) != 0) return -1;
    size_t mask = c->sent_chunks.cap - 1;
    size_t idx = (size_t)hash_u64((uint64_t)key) & mask;
    size_t first_tomb = (size_t)-1;
    for (;;) {
        uint8_t st = c->sent_chunks.states[idx];
        if (st == 0) {
            size_t use = (first_tomb != (size_t)-1) ? first_tomb : idx;
            if (first_tomb != (size_t)-1) c->sent_chunks.tombs--;
            c->sent_chunks.states[use] = 1;
            c->sent_chunks.keys[use] = key;
            c->sent_chunks.len++;
            return 0;
        }
        if (st == 2 && first_tomb == (size_t)-1) {
            first_tomb = idx;
        } else if (st == 1 && c->sent_chunks.keys[idx] == key) {
            return 0;
        }
        idx = (idx + 1) & mask;
    }
}

static int sent_chunks_remove(mc_conn_t *c, int64_t key) {
    if (!c || !c->sent_chunks.cap) return -1;
    size_t mask = c->sent_chunks.cap - 1;
    size_t idx = (size_t)hash_u64((uint64_t)key) & mask;
    for (;;) {
        uint8_t st = c->sent_chunks.states[idx];
        if (st == 0) return 0;
        if (st == 1 && c->sent_chunks.keys[idx] == key) {
            c->sent_chunks.states[idx] = 2;
            c->sent_chunks.len--;
            c->sent_chunks.tombs++;
            return 0;
        }
        idx = (idx + 1) & mask;
    }
}

static int pending_chunks_init(mc_conn_t *c, size_t cap_hint) {
    if (!c) return -1;
    if (c->pending_chunks.cap != 0) return 0;
    size_t cap = cap_hint ? cap_hint : 1024;
    c->pending_chunks.items = calloc(cap, sizeof(*c->pending_chunks.items));
    if (!c->pending_chunks.items) return -1;
    c->pending_chunks.cap = cap;
    c->pending_chunks.head = 0;
    c->pending_chunks.len = 0;
    return 0;
}

static void pending_chunks_clear(mc_conn_t *c) {
    if (!c) return;
    c->pending_chunks.head = 0;
    c->pending_chunks.len = 0;
}

static size_t pending_chunks_count(const mc_conn_t *c) {
    if (!c) return 0;
    if (c->pending_chunks.len < c->pending_chunks.head) return 0;
    return c->pending_chunks.len - c->pending_chunks.head;
}

static int pending_chunks_push(mc_conn_t *c, int32_t cx, int32_t cz, uint32_t prio) {
    if (!c) return -1;
    if (c->pending_chunks.len == c->pending_chunks.cap) {
        size_t new_cap = c->pending_chunks.cap ? c->pending_chunks.cap * 2 : 1024;
        void *next = realloc(c->pending_chunks.items, new_cap * sizeof(*c->pending_chunks.items));
        if (!next) return -1;
        c->pending_chunks.items = next;
        c->pending_chunks.cap = new_cap;
    }
    c->pending_chunks.items[c->pending_chunks.len++] = (mc_chunk_req_t){cx, cz, prio};
    return 0;
}

static bool pending_chunks_pop(mc_conn_t *c, mc_chunk_req_t *out) {
    if (!c || !out) return false;
    if (c->pending_chunks.head >= c->pending_chunks.len) return false;
    *out = c->pending_chunks.items[c->pending_chunks.head++];
    if (c->pending_chunks.head == c->pending_chunks.len) {
        c->pending_chunks.head = 0;
        c->pending_chunks.len = 0;
    }
    return true;
}

static int send_unload_chunk(mc_conn_t *c, int32_t cx, int32_t cz) {
    uint8_t buf[8];
    size_t pos = 0;
    if (w_i32(buf, sizeof(buf), &pos, cz) != 0) return -1;
    if (w_i32(buf, sizeof(buf), &pos, cx) != 0) return -1;
    return conn_write_packet(c, PKT_PLAY_UNLOAD_CHUNK, buf, pos, -1);
}

static int send_center_chunk(mc_conn_t *c, int32_t cx, int32_t cz) {
    uint8_t buf[32];
    size_t pos = 0;
    if (w_varint(buf, sizeof(buf), &pos, cx) != 0) return -1;
    if (w_varint(buf, sizeof(buf), &pos, cz) != 0) return -1;
    return conn_write_packet(c, PKT_PLAY_SET_CENTER_CHUNK, buf, pos, -1);
}

static int send_chunk_ready(mc_conn_t *c, mc_world_t *world, int32_t cx, int32_t cz, const mc_chunk_t *chunk) {
    if (!c || !world || !chunk) return -1;
    bool perf = mc_perf_enabled();
    int64_t perf_start_us = perf ? mc_now_us() : 0;
    int64_t span_start_us = 0;
    int64_t chunkdata_us = 0;
    int64_t heightmap_us = 0;
    int64_t block_entity_scan_us = 0;
    int64_t payload_us = 0;
    int64_t write_queue_us = 0;
    int64_t refresh_ping_us = 0;

    mc_buf_t chunkdata;
    if (buf_init(&chunkdata, 32 * 1024) != 0) return -1;
    /* Serialize the authoritative live chunk from RAM. The captured template
     * only contributes heightmaps/biomes fallback/light payload, not block
     * state storage. */
    span_start_us = perf ? mc_now_us() : 0;
    int rc = proto_play_encode_chunkdata_for_test(world, chunk, &chunkdata);
    if (perf) chunkdata_us = mc_now_us() - span_start_us;
    if (rc != 0) {
        buf_free(&chunkdata);
        return -1;
    }

    mc_buf_t payload;
    if (buf_init(&payload, 64 * 1024) != 0) {
        buf_free(&chunkdata);
        return -1;
    }
    mc_buf_t block_entities;
    if (buf_init(&block_entities, 512) != 0) {
        buf_free(&payload);
        buf_free(&chunkdata);
        return -1;
    }
    int32_t block_entity_count = 0;
    uint8_t *hm = NULL;
    size_t hm_len = 0;
    span_start_us = perf ? mc_now_us() : 0;
    if (build_motion_blocking_heightmaps(world, chunk, &hm, &hm_len) != 0) {
        hm = NULL;
        hm_len = 0;
    }
    if (perf) heightmap_us = mc_now_us() - span_start_us;

    span_start_us = perf ? mc_now_us() : 0;
    for (int y_index = 0; y_index < MC_WORLD_HEIGHT; y_index++) {
        int32_t world_y = MC_WORLD_MIN_Y + y_index;
        for (int lz = 0; lz < MC_CHUNK_XZ; lz++) {
            for (int lx = 0; lx < MC_CHUNK_XZ; lx++) {
                int32_t state_id = mc_world_normalize_container_state_id((int32_t)mc_chunk_get_block(chunk, lx, world_y, lz));
                int32_t be_type = container_block_entity_type(state_id);
                if (be_type < 0) continue;

                uint8_t *nbt = NULL;
                size_t nbt_len = 0;
                int32_t world_x = cx * MC_CHUNK_XZ + lx;
                int32_t world_z = cz * MC_CHUNK_XZ + lz;
                if (container_block_entity_chunk_nbt(world, state_id, world_x, world_y, world_z, &nbt, &nbt_len) != 0) {
                    buf_free(&block_entities);
                    free(hm);
                    buf_free(&payload);
                    buf_free(&chunkdata);
                    return -1;
                }

                uint8_t packed_xz = (uint8_t)((((world_x & 15) << 4) | (world_z & 15)) & 0xFF);
                if (buf_w_u8(&block_entities, packed_xz) != 0 || buf_w_u16_be(&block_entities, (uint16_t)world_y) != 0 ||
                    buf_w_varint(&block_entities, be_type) != 0 || buf_write(&block_entities, nbt, nbt_len) != 0) {
                    free(nbt);
                    buf_free(&block_entities);
                    free(hm);
                    buf_free(&payload);
                    buf_free(&chunkdata);
                    return -1;
                }
                free(nbt);
                block_entity_count++;
                if (debug_container_pos_match(world_x, world_y, world_z)) {
                    log_info("containers debug: send_chunk_ready chunk=(%d,%d) pos=(%d,%d,%d) state_id=%d key=%s", cx, cz, world_x,
                             world_y, world_z, state_id, mc_block_state_key(state_id) ? mc_block_state_key(state_id) : "(null)");
                }
            }
        }
    }
    if (perf) block_entity_scan_us = mc_now_us() - span_start_us;

    const uint8_t *light = g_chunk_tpl.fullbright_light ? g_chunk_tpl.fullbright_light : g_chunk_tpl.light;
    size_t light_len = g_chunk_tpl.fullbright_light ? g_chunk_tpl.fullbright_light_len : g_chunk_tpl.light_len;
    span_start_us = perf ? mc_now_us() : 0;
    if (buf_w_i32_be(&payload, cx) != 0 || buf_w_i32_be(&payload, cz) != 0 ||
        buf_write(&payload, hm ? hm : g_chunk_tpl.heightmaps, hm ? hm_len : g_chunk_tpl.heightmaps_len) != 0 ||
        buf_w_varint(&payload, (int32_t)chunkdata.len) != 0 ||
        buf_write(&payload, chunkdata.data, chunkdata.len) != 0 ||
        buf_w_varint(&payload, block_entity_count) != 0 ||
        buf_write(&payload, block_entities.data, block_entities.len) != 0 ||
        buf_write(&payload, light, light_len) != 0) {
        buf_free(&block_entities);
        free(hm);
        buf_free(&payload);
        buf_free(&chunkdata);
        return -1;
    }
    if (perf) payload_us = mc_now_us() - span_start_us;

    size_t perf_payload_len = payload.len;
    size_t perf_chunkdata_len = chunkdata.len;
    size_t perf_block_entities_len = block_entities.len;
    size_t perf_heightmaps_len = hm ? hm_len : g_chunk_tpl.heightmaps_len;
    size_t perf_light_len = light_len;

    span_start_us = perf ? mc_now_us() : 0;
    rc = conn_write_packet(c, PKT_PLAY_CHUNK_DATA, payload.data, payload.len, -1);
    if (perf) write_queue_us = mc_now_us() - span_start_us;
    buf_free(&block_entities);
    free(hm);
    buf_free(&payload);
    buf_free(&chunkdata);
    if (rc == 0) {
        span_start_us = perf ? mc_now_us() : 0;
        rc = maybe_send_chunk_refresh_ping(c);
        if (perf) refresh_ping_us = mc_now_us() - span_start_us;
    }
    if (perf) {
        int64_t elapsed_us = mc_now_us() - perf_start_us;
        if (elapsed_us >= mc_perf_slow_us() || rc != 0) {
            log_info("perf chunk_ready: chunk=(%d,%d) elapsed=%.3fms encode=%.3fms heightmap=%.3fms block_entities_scan=%.3fms payload_build=%.3fms write_queue=%.3fms refresh_ping=%.3fms payload=%zu chunkdata=%zu block_entities=%d block_entity_bytes=%zu heightmaps=%zu light=%zu rc=%d",
                     cx,
                     cz,
                     (double)elapsed_us / 1000.0,
                     (double)chunkdata_us / 1000.0,
                     (double)heightmap_us / 1000.0,
                     (double)block_entity_scan_us / 1000.0,
                     (double)payload_us / 1000.0,
                     (double)write_queue_us / 1000.0,
                     (double)refresh_ping_us / 1000.0,
                     perf_payload_len,
                     perf_chunkdata_len,
                     block_entity_count,
                     perf_block_entities_len,
                     perf_heightmaps_len,
                     perf_light_len,
                     rc);
        }
    }
    return rc;
}

static int send_sent_chunks_post_updates(mc_conn_t *c) {
    if (!c) return -1;
    mc_world_t *world = get_world(c);
    if (!world) return -1;
    if (!c->sent_chunks.cap || !c->sent_chunks.states) return 0;

    for (size_t i = 0; i < c->sent_chunks.cap; i++) {
        if (c->sent_chunks.states[i] != 1) continue;
        int64_t key = c->sent_chunks.keys[i];
        int32_t cx = key_cx(key);
        int32_t cz = key_cz(key);
        mc_chunk_t *chunk = mc_world_get_chunk(world, cx, cz, UINT32_MAX);
        if (!chunk) continue;

        for (int y_index = 0; y_index < MC_WORLD_HEIGHT; y_index++) {
            int32_t world_y = MC_WORLD_MIN_Y + y_index;
            for (int lz = 0; lz < MC_CHUNK_XZ; lz++) {
                for (int lx = 0; lx < MC_CHUNK_XZ; lx++) {
                    int32_t state_id = mc_world_normalize_container_state_id((int32_t)mc_chunk_get_block(chunk, lx, world_y, lz));
                    int32_t be_type = container_block_entity_type(state_id);
                    if (be_type < 0) continue;
                    int32_t world_x = cx * MC_CHUNK_XZ + lx;
                    int32_t world_z = cz * MC_CHUNK_XZ + lz;
                    if (send_block_update_packet(c, world_x, world_y, world_z, state_id) != 0) return -1;
                    if (send_block_entity_data_packet(c, state_id, world_x, world_y, world_z) != 0) return -1;
                }
            }
        }
    }
    return 0;
}

static int maybe_send_chunk_refresh_ping(mc_conn_t *c) {
    if (!c) return -1;
    if (c->chunk_refresh_ping_pending) return 0;

    uint8_t buf[8];
    size_t pos = 0;
    c->chunk_refresh_ping_id = CHUNK_REFRESH_PING_ID;
    if (w_i32(buf, sizeof(buf), &pos, c->chunk_refresh_ping_id) != 0) return -1;
    if (conn_write_packet(c, PKT_PLAY_CLIENTBOUND_PING, buf, pos, -1) != 0) return -1;
    c->chunk_refresh_ping_pending = true;
    return 0;
}

static int32_t floor_i32_from_f64(double v) {
    int64_t i = (int64_t)v;
    if ((double)i > v) i--;
    if (i < INT32_MIN) return INT32_MIN;
    if (i > INT32_MAX) return INT32_MAX;
    return (int32_t)i;
}

static int32_t block_to_chunk(int32_t b) {
    return (b >= 0) ? (b / CHUNK_XZ) : ((b - (CHUNK_XZ - 1)) / CHUNK_XZ);
}

static int rebuild_chunk_stream(mc_conn_t *c, mc_world_t *world, int32_t center_cx, int32_t center_cz, int view) {
    if (!c || !world) return -1;
    if (send_center_chunk(c, center_cx, center_cz) != 0) return -1;

    for (size_t i = 0; i < c->sent_chunks.cap; i++) {
        if (c->sent_chunks.states[i] != 1) continue;
        int64_t key = c->sent_chunks.keys[i];
        int32_t cx = key_cx(key);
        int32_t cz = key_cz(key);
        int dx = cx - center_cx;
        int dz = cz - center_cz;
        if (dx < 0) dx = -dx;
        if (dz < 0) dz = -dz;
        if (dx <= view && dz <= view) continue;
        if (send_unload_chunk(c, cx, cz) != 0) return -1;
        (void)sent_chunks_remove(c, key);
    }

    pending_chunks_clear(c);

    for (int r = 0; r <= view; r++) {
        for (int dz = -r; dz <= r; dz++) {
            for (int dx = -r; dx <= r; dx++) {
                int adx = dx < 0 ? -dx : dx;
                int adz = dz < 0 ? -dz : dz;
                if (adx != r && adz != r) continue;
                int32_t cx = center_cx + dx;
                int32_t cz = center_cz + dz;
                int64_t key = chunk_key(cx, cz);
                if (sent_chunks_contains(c, key)) continue;
                if (pending_chunks_push(c, cx, cz, (uint32_t)r) != 0) return -1;
                (void)mc_world_get_chunk(world, cx, cz, (uint32_t)r);
            }
        }
    }

    c->has_center_chunk = true;
    c->center_cx = center_cx;
    c->center_cz = center_cz;
    return 0;
}

static int chunk_stream_tick(mc_conn_t *c) {
    if (!c || c->state != MC_STATE_PLAY || !c->cfg) return 0;
    mc_world_t *world = get_world(c);
    if (!world) return -1;
    bool perf = mc_perf_enabled();
    int64_t perf_start_us = perf ? mc_now_us() : 0;

    int view = c->cfg->view_distance;
    if (view < 2) view = 2;
    if (view > 32) view = 32;

    if (sent_chunks_init(c, 1024) != 0) return -1;
    if (pending_chunks_init(c, 1024) != 0) return -1;

    int32_t pcx = c->center_cx;
    int32_t pcz = c->center_cz;
    if (c->has_pos) {
        int32_t bx = floor_i32_from_f64(c->x);
        int32_t bz = floor_i32_from_f64(c->z);
        pcx = block_to_chunk(bx);
        pcz = block_to_chunk(bz);
    }

    if (!c->has_center_chunk || pcx != c->center_cx || pcz != c->center_cz) {
        if (rebuild_chunk_stream(c, world, pcx, pcz, view) != 0) return -1;
    }

    size_t pending_start = pending_chunks_count(c);
    int sent = 0;
    int scans = 0;
    int misses = 0;
    while (sent < CHUNKS_PER_TICK) {
        if (pending_chunks_count(c) == 0) break;
        mc_chunk_req_t req;
        if (!pending_chunks_pop(c, &req)) break;

        mc_chunk_t *chunk = mc_world_get_chunk(world, req.cx, req.cz, req.prio);
        if (!chunk) {
            if (pending_chunks_push(c, req.cx, req.cz, req.prio) != 0) return -1;
            misses++;
            scans++;
            if (scans >= CHUNK_SEND_SCAN_LIMIT) break;
            continue;
        }

        if (send_chunk_ready(c, world, req.cx, req.cz, chunk) != 0) return -1;
        if (sent_chunks_insert(c, chunk_key(req.cx, req.cz)) != 0) return -1;
        sent++;
    }

    if (perf) {
        int64_t elapsed_us = mc_now_us() - perf_start_us;
        if (elapsed_us >= mc_perf_slow_us()) {
            log_info("perf chunk_stream: player=%s elapsed=%.3fms sent=%d scans=%d misses=%d pending_start=%zu pending_end=%zu view=%d center=(%d,%d)",
                     c->username[0] ? c->username : "(unknown)",
                     (double)elapsed_us / 1000.0,
                     sent,
                     scans,
                     misses,
                     pending_start,
                     pending_chunks_count(c),
                     view,
                     pcx,
                     pcz);
        }
    }

    return 0;
}

static int send_waiting_for_level_chunks_event(mc_conn_t *c) {
    uint8_t buf[8];
    size_t pos = 0;
    if (w_ubyte(buf, sizeof(buf), &pos, 13) != 0) return -1;
    if (w_f32(buf, sizeof(buf), &pos, 0.0f) != 0) return -1;
    return conn_write_packet(c, PKT_PLAY_GAME_EVENT, buf, pos, -1);
}

static int send_default_spawn_packet(mc_conn_t *c, double x, double y, double z, float yaw, float pitch) {
    uint8_t buf[32];
    size_t pos = 0;
    /* 26.1 ClientboundSetDefaultSpawnPositionPacket carries LevelData.RespawnData:
     * GlobalPos(dimension id + block pos) + yaw + pitch. The client now decodes the
     * dimension portion as a compact id before the BlockPos payload. */
    if (w_varint(buf, sizeof(buf), &pos, 0) != 0) return -1; /* overworld dimension id */
    if (w_position(buf, sizeof(buf), &pos, floor_i32_from_f64(x), floor_i32_from_f64(y), floor_i32_from_f64(z)) != 0) return -1;
    if (w_f32(buf, sizeof(buf), &pos, yaw) != 0) return -1;
    if (w_f32(buf, sizeof(buf), &pos, pitch) != 0) return -1;
    return conn_write_packet(c, PKT_PLAY_SET_DEFAULT_SPAWN, buf, pos, -1);
}

static void reset_remote_player_tracking(mc_conn_t *c) {
    if (!c) return;
    c->remote_players_len = 0;
}

static int send_respawn_packet(mc_conn_t *c) {
    if (!c) return -1;
    uint8_t buf[128];
    size_t pos = 0;
    int64_t level_seed = (c->cfg) ? c->cfg->level_seed : 0;

    if (w_varint(buf, sizeof(buf), &pos, 0) != 0) return -1; /* dimension type id */
    if (w_string(buf, sizeof(buf), &pos, "minecraft:overworld") != 0) return -1;
    if (w_i64(buf, sizeof(buf), &pos, level_seed) != 0) return -1;
    if (w_ubyte(buf, sizeof(buf), &pos, (uint8_t)c->gamemode) != 0) return -1;
    if (w_byte(buf, sizeof(buf), &pos, -1) != 0) return -1;
    if (w_bool(buf, sizeof(buf), &pos, false) != 0) return -1; /* is debug */
    if (w_bool(buf, sizeof(buf), &pos, true) != 0) return -1;  /* is flat */
    if (w_bool(buf, sizeof(buf), &pos, false) != 0) return -1; /* has death location */
    if (w_varint(buf, sizeof(buf), &pos, 0) != 0) return -1;   /* portal cooldown */
    if (w_varint(buf, sizeof(buf), &pos, 63) != 0) return -1;  /* sea level */
    if (w_ubyte(buf, sizeof(buf), &pos, PLAYER_RESPAWN_COPY_METADATA) != 0) return -1;
    return conn_write_packet(c, PKT_PLAY_RESPAWN, buf, pos, -1);
}

static int respawn_player(mc_conn_t *c) {
    if (!c || !c->player) return -1;

    mc_world_t *world = get_world(c);
    if (!world) return -1;

    double spawn_x = 0.0;
    double spawn_y = 0.0;
    double spawn_z = 0.0;
    float spawn_yaw = 0.0f;
    float spawn_pitch = 0.0f;
    world_spawn_values(&spawn_x, &spawn_y, &spawn_z, &spawn_yaw, &spawn_pitch);

    int view_distance = (c->cfg && c->cfg->view_distance >= 2) ? c->cfg->view_distance : 10;
    int32_t spawn_chunk_x = block_to_chunk(floor_i32_from_f64(spawn_x));
    int32_t spawn_chunk_z = block_to_chunk(floor_i32_from_f64(spawn_z));

    if (send_respawn_packet(c) != 0) return -1;
    if (send_waiting_for_level_chunks_event(c) != 0) return -1;

    cancel_item_use(c);
    c->dead = false;
    c->health = PLAYER_MAX_HEALTH;
    c->food = PLAYER_DEFAULT_FOOD_LEVEL;
    c->food_saturation = PLAYER_DEFAULT_FOOD_SATURATION;
    c->food_exhaustion = 0.0f;
    c->player->health = PLAYER_MAX_HEALTH;
    c->player->food_level = c->food;
    c->player->food_saturation = c->food_saturation;
    c->player->food_exhaustion = c->food_exhaustion;
    c->player->pos_x = spawn_x;
    c->player->pos_y = spawn_y;
    c->player->pos_z = spawn_z;
    c->player->yaw = spawn_yaw;
    c->player->pitch = spawn_pitch;
    c->has_center_chunk = false;
    c->chunk_refresh_ping_pending = false;
    c->chunk_refresh_ping_id = 0;
    c->next_natural_regen_ms = 0;
    c->next_starvation_damage_ms = 0;
    reset_fall_tracking(c);
    reset_remote_player_tracking(c);
    sent_chunks_clear(c);
    pending_chunks_clear(c);

    if (send_default_spawn_packet(c, spawn_x, spawn_y, spawn_z, spawn_yaw, spawn_pitch) != 0) return -1;
    if (send_sync_position(c, spawn_x, spawn_y, spawn_z, spawn_yaw, spawn_pitch) != 0) return -1;
    if (rebuild_chunk_stream(c, world, spawn_chunk_x, spawn_chunk_z, view_distance) != 0) return -1;
    if (chunk_stream_tick(c) != 0) return -1;
    if (send_player_abilities(c) != 0) return -1;
    if (send_set_health_packet(c) != 0) return -1;
    if (sync_inventory_full(c) != 0) return -1;
    if (c->server && net_server_sync_item_entities_to_conn(c->server, c) != 0) return -1;
    return save_player_data(c);
}

int proto_send_play_disconnect(mc_conn_t *c, const char *reason_json) {
    if (!c) return -1;
    const char *msg = reason_json ? reason_json : "{\"text\":\"Play disconnect\"}";

    uint8_t buf[512];
    size_t pos = 0;
    if (w_string(buf, sizeof(buf), &pos, msg) != 0) return -1;

    if (conn_write_packet(c, PKT_PLAY_DISCONNECT, buf, pos, -1) != 0) return -1;
    c->closing = true;
    return 0;
}

int proto_play_send_initial(mc_conn_t *c) {
    if (!c || c->play_init_sent) return 0;
    if (!get_world(c)) return -1;
    if (ensure_player_loaded(c) != 0) return -1;
    c->gamemode = c->player ? c->player->gamemode : GAMEMODE_CREATIVE;
    int view_distance = (c->cfg && c->cfg->view_distance >= 2) ? c->cfg->view_distance : 10;
    int simulation_distance = (c->cfg && c->cfg->simulation_distance >= 2) ? c->cfg->simulation_distance : 10;
    int64_t level_seed = (c->cfg) ? c->cfg->level_seed : 0;

    uint8_t buf[1024];
    size_t pos = 0;

    if (w_i32(buf, sizeof(buf), &pos, c->entity_id) != 0) return -1;
    if (w_bool(buf, sizeof(buf), &pos, false) != 0) return -1; /* is hardcore */
    if (w_varint(buf, sizeof(buf), &pos, 3) != 0) return -1;   /* dimension count */
    if (w_string(buf, sizeof(buf), &pos, "minecraft:overworld") != 0) return -1;
    if (w_string(buf, sizeof(buf), &pos, "minecraft:the_nether") != 0) return -1;
    if (w_string(buf, sizeof(buf), &pos, "minecraft:the_end") != 0) return -1;
    if (w_varint(buf, sizeof(buf), &pos, 100) != 0) return -1; /* max players */
    if (w_varint(buf, sizeof(buf), &pos, view_distance) != 0) return -1;  /* view distance */
    if (w_varint(buf, sizeof(buf), &pos, simulation_distance) != 0) return -1;  /* simulation distance */
    if (w_bool(buf, sizeof(buf), &pos, false) != 0) return -1; /* reduced debug info */
    if (w_bool(buf, sizeof(buf), &pos, true) != 0) return -1;  /* enable respawn screen */
    if (w_bool(buf, sizeof(buf), &pos, false) != 0) return -1; /* do limited crafting */
    if (w_varint(buf, sizeof(buf), &pos, 0) != 0) return -1;   /* dimension type id */
    if (w_string(buf, sizeof(buf), &pos, "minecraft:overworld") != 0) return -1;
    if (w_i64(buf, sizeof(buf), &pos, level_seed) != 0) return -1;      /* hashed seed */
    if (w_ubyte(buf, sizeof(buf), &pos, (uint8_t)c->gamemode) != 0) return -1; /* game mode */
    if (w_byte(buf, sizeof(buf), &pos, -1) != 0) return -1;    /* previous game mode */
    if (w_bool(buf, sizeof(buf), &pos, false) != 0) return -1; /* is debug */
    if (w_bool(buf, sizeof(buf), &pos, true) != 0) return -1;  /* is flat */
    if (w_bool(buf, sizeof(buf), &pos, false) != 0) return -1; /* has death location */
    if (w_varint(buf, sizeof(buf), &pos, 0) != 0) return -1;   /* portal cooldown */
    if (w_varint(buf, sizeof(buf), &pos, 63) != 0) return -1;  /* sea level */
    if (w_bool(buf, sizeof(buf), &pos, false) != 0) return -1; /* enforces secure chat */

    if (conn_write_packet(c, PKT_PLAY_LOGIN, buf, pos, -1) != 0) return -1;
    if (proto_play_send_difficulty(c) != 0) return -1;

    if (send_waiting_for_level_chunks_event(c) != 0) return -1;

    mc_world_t *world = get_world(c);
    if (!world) return -1;
    if (load_chunk_template(c) != 0) {
        return proto_send_play_disconnect(c, "{\"text\":\"Chunk template load failed\"}");
    }
    if (sent_chunks_init(c, 1024) != 0 || pending_chunks_init(c, 1024) != 0) return -1;
    sent_chunks_clear(c);
    pending_chunks_clear(c);

    /* 26.1 default spawn packet carries LevelData.RespawnData:
     * GlobalPos(dimension + block pos) + yaw + pitch. */
    double spawn_x = (c->player ? c->player->pos_x : 0.5);
    double spawn_y = (c->player ? c->player->pos_y : WORLD_SPAWN_Y);
    double spawn_z = (c->player ? c->player->pos_z : 0.5);
    float spawn_yaw = (c->player ? c->player->yaw : 0.0f);
    float spawn_pitch = (c->player ? c->player->pitch : 0.0f);
    int32_t spawn_chunk_x = block_to_chunk(floor_i32_from_f64(spawn_x));
    int32_t spawn_chunk_z = block_to_chunk(floor_i32_from_f64(spawn_z));

    c->x = spawn_x;
    c->y = spawn_y;
    c->z = spawn_z;
    c->yaw = spawn_yaw;
    c->pitch = spawn_pitch;
    c->has_pos = true;
    c->has_center_chunk = false;
    if (rebuild_chunk_stream(c, world, spawn_chunk_x, spawn_chunk_z, view_distance) != 0) return -1;

    if (send_default_spawn_packet(c, spawn_x, spawn_y, spawn_z, spawn_yaw, spawn_pitch) != 0) return -1;

    /* Synchronize Player Position */
    if (send_sync_position(c, spawn_x, spawn_y, spawn_z, spawn_yaw, spawn_pitch) != 0) return -1;
    if (chunk_stream_tick(c) != 0) return -1;

    if (send_player_abilities(c) != 0) return -1;
    if (send_set_health_packet(c) != 0) return -1;
    if (sync_inventory_full(c) != 0) return -1;
    if (send_entity_event(c, ENTITY_STATUS_OP_LEVEL_4) != 0) return -1;
    if (send_commands(c) != 0) return -1;

    c->play_init_sent = true;
    c->play_ready = false;
    return 0;
}

static int handle_player_movement_update(mc_conn_t *c, bool has_pos, double x, double y, double z, bool has_rot, float yaw, float pitch,
                                         bool on_ground, int64_t now_ms) {
    if (!c) return -1;

    bool prev_on_ground = c->on_ground;
    bool prev_has_pos = c->has_pos;
    double prev_x = c->x;
    double prev_y = c->y;
    double prev_z = c->z;
    bool jump_started = false;
    if (has_pos) {
        c->x = x;
        c->y = y;
        c->z = z;
    }
    if (has_rot) {
        c->yaw = yaw;
        c->pitch = pitch;
    }
    c->has_pos = true;
    c->on_ground = on_ground;

    if (player_can_use_hunger_system(c) && has_pos && prev_has_pos) {
        double dx = x - prev_x;
        double dz = z - prev_z;
        double horizontal_distance = sqrt(dx * dx + dz * dz);
        if (horizontal_distance > PLAYER_MIN_MOVEMENT_SAMPLE && horizontal_distance <= PLAYER_MAX_HUNGER_SAMPLE_DISTANCE) {
            float multiplier = horizontal_distance >= PLAYER_SPRINT_DISTANCE_THRESHOLD ? PLAYER_SPRINT_EXHAUSTION_PER_BLOCK
                                                                                       : PLAYER_MOVE_EXHAUSTION_PER_BLOCK;
            if (player_add_exhaustion(c, (float)(horizontal_distance * multiplier), false) != 0) return -1;
        }
        if (prev_on_ground && !on_ground && y > prev_y + PLAYER_JUMP_MIN_ASCENT) {
            jump_started = true;
        }
    }

    if (jump_started) {
        if (player_add_exhaustion(c, PLAYER_JUMP_EXHAUSTION, false) != 0) return -1;
    }
    if (!player_can_take_damage(c)) {
        c->fall_tracking = false;
        return 0;
    }

    if (!on_ground) {
        if (!c->fall_tracking || prev_on_ground) {
            c->fall_tracking = true;
            c->fall_start_y = c->y;
        } else if (c->y > c->fall_start_y) {
            c->fall_start_y = c->y;
        }
        return 0;
    }

    if (c->fall_tracking && !prev_on_ground) {
        double fall_distance = c->fall_start_y - c->y;
        c->fall_tracking = false;
        if (fall_distance > PLAYER_FALL_SAFE_DISTANCE) {
            float damage = (float)((int)(fall_distance - PLAYER_FALL_SAFE_DISTANCE));
            if (damage < 1.0f) damage = 1.0f;
            return apply_player_damage(c, damage, "You fell from a high place", false, now_ms);
        }
        return 0;
    }

    c->fall_tracking = false;
    return 0;
}

int proto_play_handle(mc_conn_t *c, const mc_frame_t *frame, int64_t now_ms) {
    if (!c || !frame) return -1;

    if (frame->packet_id == PKT_PLAY_CONFIRM_TELEPORT) {
        int32_t teleport_id = 0;
        size_t n = 0;
        if (varint_read(frame->payload.data, frame->payload.len, &teleport_id, &n) != 0) return -1;
        if (teleport_id == c->teleport_id) {
            bool became_ready = !c->play_ready;
            c->play_ready = true;
            if (became_ready && c->server && net_server_sync_item_entities_to_conn(c->server, c) != 0) {
                return -1;
            }
        }
        return 0;
    }

    if (frame->packet_id == PKT_PLAY_KEEPALIVE_SB) {
        if (frame->payload.len != 8) return -1;
        uint64_t v = 0;
        for (size_t i = 0; i < 8; i++) {
            v = (v << 8) | frame->payload.data[i];
        }
        if (c->awaiting_keepalive && (int64_t)v == c->keepalive_id) {
            c->awaiting_keepalive = false;
            c->last_keepalive_sent_ms = now_ms;
        }
        return 0;
    }

    if (frame->packet_id == PKT_PLAY_PONG_SB) {
        mc_reader_t r = {frame->payload.data, frame->payload.len, 0};
        int32_t pong_id = 0;
        if (r_i32(&r, &pong_id) != 0) return -1;
        if (c->chunk_refresh_ping_pending && pong_id == c->chunk_refresh_ping_id) {
            c->chunk_refresh_ping_pending = false;
            return send_sent_chunks_post_updates(c);
        }
        return 0;
    }

    if (frame->packet_id == PKT_PLAY_CLIENT_COMMAND_SB) {
        mc_reader_t r = {frame->payload.data, frame->payload.len, 0};
        int32_t action_id = -1;
        if (r_varint(&r, &action_id) != 0) return -1;
        if (c->dead && action_id == PLAYER_CLIENT_COMMAND_PERFORM_RESPAWN) {
            return respawn_player(c);
        }
        return 0;
    }

    if (c->dead) {
        return 0;
    }

    if (frame->packet_id == PKT_PLAY_USE_ITEM) {
        mc_reader_t r = {frame->payload.data, frame->payload.len, 0};
        int32_t hand = 0;
        int32_t sequence = 0;
        float y_rot = 0.0f;
        float x_rot = 0.0f;
        (void)y_rot;
        (void)x_rot;

        if (r_varint(&r, &hand) != 0) return -1;
        if (r_varint(&r, &sequence) != 0) return -1;
        if (r_f32(&r, &y_rot) != 0) return -1;
        if (r_f32(&r, &x_rot) != 0) return -1;
        if (hand != 0) return 0;

        {
            int rc = try_consume_selected_food(c);
            if (rc < 0) return -1;
            return 0;
        }
    }

    if (frame->packet_id == PKT_PLAY_HELD_ITEM_SLOT_SB) {
        mc_inventory_t *inv = conn_inventory(c);
        if (!inv) return -1;
        mc_reader_t r = {frame->payload.data, frame->payload.len, 0};
        int16_t slot_id = 0;
        if (r_i16(&r, &slot_id) != 0) return -1;
        if (slot_id >= 0 && slot_id < MC_PLAYER_HOTBAR_SIZE) {
            cancel_item_use(c);
            inv->selected_hotbar_slot = (int32_t)slot_id;
            inv->state_id++;
            if (debug_place_enabled()) {
                const mc_slot_t *slot = mc_inventory_selected_slot_const(inv);
                int32_t sid = proto_play_slot_to_state(mc_world_ids(get_world(c)), slot);
                const char *item_name = (slot && slot->present) ? mc_minecraft_item_name(slot->item_id) : NULL;
                log_info("place debug: held slot=%d item_id=%d item=%s state_id=%d", inv->selected_hotbar_slot,
                         (slot && slot->present) ? slot->item_id : -1, item_name ? item_name : "(empty)", sid);
            }
            (void)save_player_data(c);
        }
        return 0;
    }

    if (frame->packet_id == PKT_PLAY_SET_CREATIVE_SLOT) {
        return handle_set_creative_slot(c, frame);
    }

    if (frame->packet_id == PKT_PLAY_WINDOW_CLICK) {
        return handle_window_click(c, frame);
    }

    if (frame->packet_id == PKT_PLAY_CLOSE_WINDOW_SB) {
        mc_reader_t r = {frame->payload.data, frame->payload.len, 0};
        int32_t window_id = 0;
        if (r_varint(&r, &window_id) == 0) {
            if (window_id == 0) {
                int return_rc = return_player_crafting_grid(c);
                if (return_rc < 0) {
                    log_error("player inventory close failed: could not return 2x2 crafting grid");
                    return -1;
                }
                if (sync_inventory_after_crafting_close(c) != 0) {
                    log_error("crafting close sync failed for player inventory menu");
                }
                if (save_player_data(c) != 0) return -1;
            } else if (c->active_window.open && c->active_window.window_id == window_id) {
                close_active_window(c, false);
            }
        } else if (c->active_window.open) {
            close_active_window(c, false);
        }
        return 0;
    }

    if (frame->packet_id == PKT_PLAY_CHAT_COMMAND || frame->packet_id == PKT_PLAY_SIGNED_CHAT_COMMAND) {
        mc_reader_t r = {frame->payload.data, frame->payload.len, 0};
        char *cmd = NULL;
        if (r_string_alloc(&r, &cmd) != 0) return -1;
        int rc = handle_command(c, cmd);
        free(cmd);
        return rc;
    }

    if (frame->packet_id == PKT_PLAY_SET_PLAYER_POSITION) {
        mc_reader_t r = {frame->payload.data, frame->payload.len, 0};
        double x = 0, y = 0, z = 0;
        bool on_ground = false;
        if (r_f64(&r, &x) != 0) return -1;
        if (r_f64(&r, &y) != 0) return -1;
        if (r_f64(&r, &z) != 0) return -1;
        if (r_bool(&r, &on_ground) != 0) return -1;
        return handle_player_movement_update(c, true, x, y, z, false, 0.0f, 0.0f, on_ground, now_ms);
    }

    if (frame->packet_id == PKT_PLAY_SET_PLAYER_POS_ROT) {
        mc_reader_t r = {frame->payload.data, frame->payload.len, 0};
        double x = 0, y = 0, z = 0;
        float yaw = 0.0f, pitch = 0.0f;
        bool on_ground = false;
        if (r_f64(&r, &x) != 0) return -1;
        if (r_f64(&r, &y) != 0) return -1;
        if (r_f64(&r, &z) != 0) return -1;
        if (r_f32(&r, &yaw) != 0) return -1;
        if (r_f32(&r, &pitch) != 0) return -1;
        if (r_bool(&r, &on_ground) != 0) return -1;
        return handle_player_movement_update(c, true, x, y, z, true, yaw, pitch, on_ground, now_ms);
    }

    if (frame->packet_id == PKT_PLAY_SET_PLAYER_ROT) {
        mc_reader_t r = {frame->payload.data, frame->payload.len, 0};
        float yaw = 0.0f, pitch = 0.0f;
        bool on_ground = false;
        if (r_f32(&r, &yaw) != 0) return -1;
        if (r_f32(&r, &pitch) != 0) return -1;
        if (r_bool(&r, &on_ground) != 0) return -1;
        return handle_player_movement_update(c, false, 0.0, 0.0, 0.0, true, yaw, pitch, on_ground, now_ms);
    }

    if (frame->packet_id == PKT_PLAY_SET_PLAYER_ON_GROUND) {
        mc_reader_t r = {frame->payload.data, frame->payload.len, 0};
        bool on_ground = false;
        if (r_bool(&r, &on_ground) != 0) return -1;
        return handle_player_movement_update(c, false, 0.0, 0.0, 0.0, false, 0.0f, 0.0f, on_ground, now_ms);
    }

    if (frame->packet_id == PKT_PLAY_PLAYER_ACTION) {
        mc_reader_t r = {frame->payload.data, frame->payload.len, 0};
        int32_t action = 0;
        int32_t x = 0, y = 0, z = 0;
        int32_t face = 0;
        int32_t seq = 0;
        if (r_varint(&r, &action) != 0) return -1;
        if (r_position(&r, &x, &y, &z) != 0) return -1;
        if (r_varint(&r, &face) != 0) return -1;
        if (r_varint(&r, &seq) != 0) return -1;
        (void)face;

        if (action == PLAYER_ACTION_DROP_ALL_ITEMS || action == PLAYER_ACTION_DROP_ITEM) {
            int rc = drop_selected_mainhand_items(c, action == PLAYER_ACTION_DROP_ALL_ITEMS ? INT_MAX : 1);
            return rc < 0 ? -1 : 0;
        }

        if (action == PLAYER_ACTION_RELEASE_USE_ITEM) {
            cancel_item_use(c);
            return 0;
        }

        if (action == PLAYER_ACTION_START_DESTROY_BLOCK || action == PLAYER_ACTION_ABORT_DESTROY_BLOCK ||
            action == PLAYER_ACTION_STOP_DESTROY_BLOCK) {
            mc_world_t *world = get_world(c);
            const mc_world_ids_t *ids = mc_world_ids(world);
            if (world && ids) {
                if (debug_place_enabled()) {
                    log_info("place debug: break action=%d pos=(%d,%d,%d) face=%d", action, x, y, z, face);
                }

                int32_t state_id = -1;
                if (mc_world_get_block(world, x, y, z, &state_id) == 0) {
                    const mc_slot_t *held_item = selected_mainhand_slot(c);

                    if (action == PLAYER_ACTION_START_DESTROY_BLOCK && c->gamemode != GAMEMODE_CREATIVE) {
                        mc_mining_session_clear(&c->mining);
                        mc_mining_break_info_t break_info = mc_mining_break_info(state_id, held_item);
                        if (!break_info.breakable) {
                            return reject_block_destroy(c, x, y, z, state_id, seq);
                        }
                        if (break_info.instant) {
                            mc_mining_session_clear(&c->mining);
                            return break_block_authoritative(c, world, ids, x, y, z, state_id, seq, break_info.can_harvest);
                        }
                        int32_t held_item_id = mc_mining_slot_item_id(held_item);
                        mc_mining_session_start(&c->mining, x, y, z, state_id, now_ms, held_item_id, &break_info);
                        return send_block_changed_ack_packet(c, seq);
                    }

                    if (action == PLAYER_ACTION_ABORT_DESTROY_BLOCK) {
                        mc_mining_session_clear(&c->mining);
                        return reject_block_destroy(c, x, y, z, state_id, seq);
                    }

                    if (action == PLAYER_ACTION_STOP_DESTROY_BLOCK && c->gamemode != GAMEMODE_CREATIVE) {
                        int64_t elapsed_ms = 0;
                        int32_t held_item_id = mc_mining_slot_item_id(held_item);
                        mc_mining_stop_result_t stop_result =
                            mc_mining_session_validate_stop(&c->mining, x, y, z, state_id, held_item_id, now_ms,
                                                            &elapsed_ms);
                        if (stop_result != MC_MINING_STOP_OK) {
                            if (debug_place_enabled()) {
                                log_info("place debug: break stop reject reason=%s elapsed=%lld required=%lld pos=(%d,%d,%d) state=%d",
                                         mc_mining_stop_result_name(stop_result), (long long)elapsed_ms,
                                         (long long)c->mining.break_info.required_ms, x, y, z, state_id);
                            }
                            mc_mining_session_clear(&c->mining);
                            return reject_block_destroy(c, x, y, z, state_id, seq);
                        }
                        bool allow_default_drop = c->mining.break_info.can_harvest;
                        mc_mining_session_clear(&c->mining);
                        return break_block_authoritative(c, world, ids, x, y, z, state_id, seq, allow_default_drop);
                    }

                    mc_mining_session_clear(&c->mining);
                    return break_block_authoritative(c, world, ids, x, y, z, state_id, seq, true);
                }
            }
            mc_mining_session_clear(&c->mining);
            return send_block_changed_ack_packet(c, seq);
        }
        return 0;
    }

    if (frame->packet_id == PKT_PLAY_USE_ITEM_ON) {
        mc_reader_t r = {frame->payload.data, frame->payload.len, 0};
        int32_t hand = 0;
        int32_t x = 0, y = 0, z = 0;
        int32_t face = 0;
        float cx = 0.0f, cy = 0.0f, cz = 0.0f;
        bool inside = false;
        int32_t seq = 0;
        bool has_sequence = false;
        if (r_varint(&r, &hand) != 0) return -1;
        if (r_position(&r, &x, &y, &z) != 0) return -1;
        if (r_varint(&r, &face) != 0) return -1;
        if (r_f32(&r, &cx) != 0) return -1;
        if (r_f32(&r, &cy) != 0) return -1;
        if (r_f32(&r, &cz) != 0) return -1;
        if (r_bool(&r, &inside) != 0) return -1;
        if (r.pos < r.len) {
            if (r_varint(&r, &seq) != 0) return -1;
            has_sequence = true;
        }
        if (hand != 0) {
            if (has_sequence && send_block_changed_ack_packet(c, seq) != 0) return -1;
            return 0;
        }
        (void)cx;
        (void)cy;
        (void)cz;
        (void)inside;

        int open_rc = try_open_target_container(c, x, y, z);
        if (open_rc != 0) {
            if (open_rc > 0 && has_sequence && send_block_changed_ack_packet(c, seq) != 0) return -1;
            return open_rc < 0 ? -1 : 0;
        }

        {
            int food_rc = try_consume_selected_food(c);
            if (food_rc < 0) return -1;
            if (food_rc > 0) {
                if (has_sequence && send_block_changed_ack_packet(c, seq) != 0) return -1;
                return 0;
            }
        }

        int32_t px = x;
        int32_t py = y;
        int32_t pz = z;
        if (face == 0) py -= 1;
        else if (face == 1) py += 1;
        else if (face == 2) pz -= 1;
        else if (face == 3) pz += 1;
        else if (face == 4) px -= 1;
        else if (face == 5) px += 1;

        mc_world_t *world = get_world(c);
        const mc_slot_t *held = selected_mainhand_slot(c);
        const mc_world_ids_t *ids = mc_world_ids(world);
        if (world && ids) {
            int held_idx = selected_mainhand_slot_index(c);
            int32_t mapped_state_id = (held && held->present) ? mc_item_default_place_state(held->item_id) : -1;
            int32_t requested_state_id = mc_resolve_placement_state(held, c->yaw, face);
            if (requested_state_id == 0 && held && held->present && held->item_id != ITEM_AIR) {
                requested_state_id = -1;
            }
            int32_t normalized_state_id = mc_world_normalize_container_state_id(requested_state_id);
            int32_t authoritative_state_id = -1;
            int32_t network_state_id = -1;
            int32_t target_state_id = -1;
            bool placed_block = false;
            if (debug_place_enabled()) {
                const char *item_name = (held && held->present) ? mc_minecraft_item_name(held->item_id) : NULL;
                log_info("place debug: use_item_on begin held_slot=%d item_id=%d item=%s yaw=%.2f target=(%d,%d,%d) face=%d placed=(%d,%d,%d) seq=%d mapped=%d requested=%d normalized=%d mapped_key=%s requested_key=%s normalized_key=%s",
                         c->player ? c->player->inventory.selected_hotbar_slot : -1,
                         held && held->present ? held->item_id : -1, item_name ? item_name : "(empty)",
                         c->yaw, x, y, z, face, px, py, pz, seq, mapped_state_id, requested_state_id, normalized_state_id,
                         mapped_state_id >= 0 && mc_block_state_key(mapped_state_id) ? mc_block_state_key(mapped_state_id) : "(none)",
                         requested_state_id >= 0 && mc_block_state_key(requested_state_id) ? mc_block_state_key(requested_state_id) : "(none)",
                         normalized_state_id >= 0 && mc_block_state_key(normalized_state_id) ? mc_block_state_key(normalized_state_id) : "(none)");
            }
            if (normalized_state_id >= 0) {
                if (mc_world_get_block(world, px, py, pz, &target_state_id) != 0) {
                    target_state_id = -1;
                }
                if (target_state_id >= 0 && !block_state_is_replaceable(ids, target_state_id)) {
                    if (send_block_update_packet(c, px, py, pz, target_state_id) != 0) return -1;
                    if (has_sequence && send_block_changed_ack_packet(c, seq) != 0) return -1;
                    if (held_idx >= 0) {
                        if (sync_inventory_slot(c, (int16_t)held_idx) != 0) return -1;
                    }
                    if (send_held_item_slot(c) != 0) return -1;
                    return 0;
                }
                if (placed_block_intersects_player(c, ids, px, py, pz, normalized_state_id)) {
                    int32_t authoritative_target = target_state_id >= 0 ? target_state_id : ids->air;
                    if (debug_place_enabled()) {
                        log_info("place debug: deny player collision player=(%.3f,%.3f,%.3f) block=(%d,%d,%d) state=%d key=%s",
                                 c->x, c->y, c->z, px, py, pz, normalized_state_id,
                                 mc_block_state_key(normalized_state_id) ? mc_block_state_key(normalized_state_id) : "(none)");
                    }
                    if (send_block_update_packet(c, px, py, pz, authoritative_target) != 0) return -1;
                    if (has_sequence && send_block_changed_ack_packet(c, seq) != 0) return -1;
                    if (held_idx >= 0) {
                        if (sync_inventory_slot(c, (int16_t)held_idx) != 0) return -1;
                    }
                    if (send_held_item_slot(c) != 0) return -1;
                    return 0;
                }
                if (mc_world_set_block(world, px, py, pz, normalized_state_id) == 0) {
                    placed_block = true;
                    authoritative_state_id = normalized_state_id;
                    if (mc_world_get_block(world, px, py, pz, &authoritative_state_id) != 0) {
                        authoritative_state_id = normalized_state_id;
                    }
                    network_state_id = authoritative_state_id;
                    if (send_block_update_packet(c, px, py, pz, network_state_id) != 0) return -1;
                    if (has_sequence && send_block_changed_ack_packet(c, seq) != 0) return -1;
                    if (c->server) {
                        (void)net_server_resolve_item_entities_for_block(c->server, px, py, pz, authoritative_state_id);
                    }
                    if (container_block_entity_type(authoritative_state_id) >= 0) {
                        if (is_world_container_state(authoritative_state_id)) {
                            mc_container_instance_t fresh_container;
                            mc_block_entity_t fresh_entity;
                            mc_container_instance_init(&fresh_container, container_kind_for_state(authoritative_state_id), px, py, pz);
                            if (block_entity_from_container_instance(&fresh_entity, container_entity_type_for_state(authoritative_state_id),
                                                                    &fresh_container) != 0 ||
                                mc_world_put_block_entity(world, px, py, pz, &fresh_entity) != 0) {
                                mc_container_instance_clear(&fresh_container);
                                return -1;
                            }
                            mc_container_instance_clear(&fresh_container);
                        }
                        if (send_block_entity_data_packet(c, authoritative_state_id, px, py, pz) != 0) return -1;
                        if (mc_world_flush_block(world, px, py, pz) != 0) return -1;
                        if (resend_authoritative_chunk_at(c, world, px, pz) != 0) return -1;
                        if (debug_container_pos_match(px, py, pz)) {
                            log_info("containers debug: place pos=(%d,%d,%d) mapped=%d requested=%d normalized=%d authoritative=%d network=%d requested_key=%s normalized_key=%s authoritative_key=%s",
                                     px, py, pz, mapped_state_id, requested_state_id, normalized_state_id, authoritative_state_id,
                                     network_state_id,
                                     mc_block_state_key(requested_state_id) ? mc_block_state_key(requested_state_id) : "(null)",
                                     mc_block_state_key(normalized_state_id) ? mc_block_state_key(normalized_state_id) : "(null)",
                                     mc_block_state_key(authoritative_state_id) ? mc_block_state_key(authoritative_state_id) : "(null)");
                        }
                    }
                    if (c->gamemode != GAMEMODE_CREATIVE && held_idx >= 0 && held && held->present) {
                        if (consume_selected_item(c) != 0) return -1;
                    }
                }
            }
            if (!placed_block) {
                if (mc_world_get_block(world, px, py, pz, &authoritative_state_id) == 0) {
                    network_state_id = authoritative_state_id;
                    if (send_block_update_packet(c, px, py, pz, network_state_id) != 0) return -1;
                }
                if (has_sequence && send_block_changed_ack_packet(c, seq) != 0) return -1;
                if (held_idx >= 0) {
                    if (sync_inventory_slot(c, (int16_t)held_idx) != 0) return -1;
                }
                if (send_held_item_slot(c) != 0) return -1;
            }
            if (debug_place_enabled()) {
                const char *item_name = (held && held->present) ? mc_minecraft_item_name(held->item_id) : NULL;
                log_info("[debug] Player placed item=%s(%d) mapped=%d requested=%d normalized=%d authoritative=%d network=%d seq=%d target=(%d,%d,%d) placed=(%d,%d,%d) mapped_key=%s requested_key=%s normalized_key=%s authoritative_key=%s network_key=%s",
                         item_name ? item_name : "(empty)",
                         held && held->present ? held->item_id : -1,
                         mapped_state_id,
                         requested_state_id,
                         normalized_state_id,
                         authoritative_state_id,
                         network_state_id,
                         seq,
                         x, y, z, px, py, pz,
                         mapped_state_id >= 0 && mc_block_state_key(mapped_state_id) ? mc_block_state_key(mapped_state_id) : "(none)",
                         requested_state_id >= 0 && mc_block_state_key(requested_state_id) ? mc_block_state_key(requested_state_id) : "(none)",
                         normalized_state_id >= 0 && mc_block_state_key(normalized_state_id) ? mc_block_state_key(normalized_state_id) : "(none)",
                         authoritative_state_id >= 0 && mc_block_state_key(authoritative_state_id) ? mc_block_state_key(authoritative_state_id) : "(none)",
                         network_state_id >= 0 && mc_block_state_key(network_state_id) ? mc_block_state_key(network_state_id) : "(none)");
            }
        }
        return 0;
    }

    return 0;
}

int proto_play_tick(mc_conn_t *c, int64_t now_ms) {
    if (!c || c->state != MC_STATE_PLAY) return 0;
    if (!c->play_init_sent) return 0;

    if (c->awaiting_keepalive) {
        if (now_ms - c->last_keepalive_sent_ms > KEEPALIVE_TIMEOUT_MS) {
            return proto_send_play_disconnect(c, "{\"text\":\"KeepAlive timeout\"}");
        }
    } else if (now_ms - c->last_keepalive_sent_ms >= KEEPALIVE_INTERVAL_MS) {
        uint8_t buf[16];
        size_t pos = 0;
        c->keepalive_id = now_ms;
        if (w_i64(buf, sizeof(buf), &pos, c->keepalive_id) != 0) return -1;
        if (conn_write_packet(c, PKT_PLAY_KEEPALIVE, buf, pos, -1) != 0) return -1;
        c->awaiting_keepalive = true;
        c->last_keepalive_sent_ms = now_ms;
    }

    if (tick_item_use(c) != 0) return -1;
    if (tick_open_furnace_container(c) != 0) return -1;

    if (player_can_take_damage(c) && c->has_pos && c->y < PLAYER_VOID_DAMAGE_Y) {
        if (c->next_void_damage_ms == 0 || now_ms >= c->next_void_damage_ms) {
            c->next_void_damage_ms = now_ms + PLAYER_VOID_DAMAGE_INTERVAL_MS;
            if (apply_player_damage(c, PLAYER_VOID_DAMAGE_AMOUNT, "You fell out of the world", true, now_ms) != 0) return -1;
        }
    } else if (!c->dead) {
        c->next_void_damage_ms = 0;
    }

    if (tick_natural_regen_and_starvation(c, now_ms) != 0) return -1;

    if (chunk_stream_tick(c) != 0) return -1;
    return 0;
}
