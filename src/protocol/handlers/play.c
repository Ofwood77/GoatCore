#include "mc_protocol.h"
#include "mc_inventory.h"
#include "mc_container_store.h"
#include "mc_nbt.h"
#include "mc_packed.h"
#include "mc_player_store.h"
#include "mc_util.h"
#include "generated_minecraft_ids.h"
#include "generated_registries.h"
#include "generated_item_place.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
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
#define PKT_PLAY_CHUNK_DATA MC_PKT_PLAY_CLIENTBOUND_LEVEL_CHUNK_WITH_LIGHT
#define PKT_PLAY_KEEPALIVE MC_PKT_PLAY_CLIENTBOUND_KEEP_ALIVE
#define PKT_PLAY_SYNC_POS MC_PKT_PLAY_CLIENTBOUND_PLAYER_POSITION
#define PKT_PLAY_PLAYER_ABILITIES MC_PKT_PLAY_CLIENTBOUND_PLAYER_ABILITIES
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
#define PKT_PLAY_TILE_ENTITY_DATA MC_PKT_PLAY_CLIENTBOUND_BLOCK_ENTITY_DATA
#define PKT_PLAY_SET_SLOT MC_PKT_PLAY_CLIENTBOUND_CONTAINER_SET_SLOT
#define PKT_PLAY_CHAT_COMMAND MC_PKT_PLAY_SERVERBOUND_CHAT_COMMAND
#define PKT_PLAY_SIGNED_CHAT_COMMAND MC_PKT_PLAY_SERVERBOUND_CHAT_COMMAND_SIGNED
#define PKT_PLAY_PLAYER_ACTION MC_PKT_PLAY_SERVERBOUND_PLAYER_ACTION
#define PKT_PLAY_HELD_ITEM_SLOT_SB MC_PKT_PLAY_SERVERBOUND_SET_CARRIED_ITEM
#define PKT_PLAY_WINDOW_CLICK MC_PKT_PLAY_SERVERBOUND_CONTAINER_CLICK
#define PKT_PLAY_CLOSE_WINDOW_SB MC_PKT_PLAY_SERVERBOUND_CONTAINER_CLOSE
#define PKT_PLAY_SET_CREATIVE_SLOT MC_PKT_PLAY_SERVERBOUND_SET_CREATIVE_MODE_SLOT
#define PKT_PLAY_USE_ITEM_ON MC_PKT_PLAY_SERVERBOUND_USE_ITEM_ON
#define PKT_PLAY_HELD_ITEM_SLOT MC_PKT_PLAY_CLIENTBOUND_SET_HELD_SLOT
#define PKT_PLAY_ENTITY_DESTROY MC_PKT_PLAY_CLIENTBOUND_REMOVE_ENTITIES
#define PKT_PLAY_ENTITY_HEAD_ROTATION MC_PKT_PLAY_CLIENTBOUND_ROTATE_HEAD
#define PKT_PLAY_SET_PLAYER_POSITION MC_PKT_PLAY_SERVERBOUND_MOVE_PLAYER_POS
#define PKT_PLAY_SET_PLAYER_POS_ROT MC_PKT_PLAY_SERVERBOUND_MOVE_PLAYER_POS_ROT
#define PKT_PLAY_SET_PLAYER_ROT MC_PKT_PLAY_SERVERBOUND_MOVE_PLAYER_ROT
#define PKT_PLAY_SET_PLAYER_ON_GROUND MC_PKT_PLAY_SERVERBOUND_MOVE_PLAYER_STATUS_ONLY
#define PKT_PLAY_ENTITY_TELEPORT MC_PKT_PLAY_CLIENTBOUND_TELEPORT_ENTITY

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

#define PLAYER_INFO_ACTION_ADD 0x01
#define PLAYER_INFO_ACTION_UPDATE_GAMEMODE 0x04
#define PLAYER_INFO_ACTION_UPDATE_LISTED 0x08
#define PLAYER_INFO_ACTION_UPDATE_LATENCY 0x10

#define MC_WINDOW_TYPE_GENERIC_9X3 2

#define ITEM_AIR 0

static mc_world_t *get_world(mc_conn_t *c);
static int buf_w_u8(mc_buf_t *b, uint8_t v);
static int buf_w_u16_be(mc_buf_t *b, uint16_t v);
static int buf_w_varint(mc_buf_t *b, int32_t v);
static void close_active_window(mc_conn_t *c, bool notify_client);
static int container_block_entity_type(int32_t state_id);
static const char *block_entity_name_for_state(int32_t state_id);
static bool is_chest_state(int32_t state_id);
static bool is_trapped_chest_state(int32_t state_id);
static bool is_ender_chest_state(int32_t state_id);
static int send_chunk_ready(mc_conn_t *c, mc_world_t *world, int32_t cx, int32_t cz, const mc_chunk_t *chunk);
static int send_chunk_container_repairs(mc_conn_t *c, const mc_chunk_t *chunk);
static int paletted_word_count_for_network(int entry_count, int bits);
static int encode_paletted_container_network(mc_buf_t *out, const int32_t *values, int entry_count, int palette_bits);

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

static int save_player_data(mc_conn_t *c) {
    mc_world_t *world = get_world(c);
    if (!c || !c->player || !world) return 0;
    const char *world_path = mc_world_path(world);
    if (!world_path || !*world_path) return 0;
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
    }
    c->player = player;
    return 0;
}

void proto_play_conn_cleanup(mc_conn_t *c) {
    if (!c || !c->player) return;
    close_active_window(c, false);
    (void)save_player_data(c);
    mc_player_data_clear(c->player);
    free(c->player);
    c->player = NULL;
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
    uint8_t buf[4096];
    size_t pos = 0;

    const int node_count = 11;
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

    /* 10: root -> [gamemode|tp|setblock] */
    {
        const int children[] = {5, 6, 9};
        if (commands_write_node(buf, sizeof(buf), &pos, 0x00, children, 3,
                                NULL, 0, CMD_PROP_NONE, 0) != 0) return -1;
    }

    if (w_varint(buf, sizeof(buf), &pos, 10) != 0) return -1; /* root index */
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

static mc_slot_t *player_visible_slot(mc_player_data_t *player, int window_slot) {
    if (!player) return NULL;
    if (window_slot >= 27 && window_slot < 54) return &player->inventory.slots[9 + (window_slot - 27)];
    if (window_slot >= 54 && window_slot < 63) return &player->inventory.slots[36 + (window_slot - 54)];
    return NULL;
}

static const mc_slot_t *player_visible_slot_const(const mc_player_data_t *player, int window_slot) {
    if (!player) return NULL;
    if (window_slot >= 27 && window_slot < 54) return &player->inventory.slots[9 + (window_slot - 27)];
    if (window_slot >= 54 && window_slot < 63) return &player->inventory.slots[36 + (window_slot - 54)];
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

static mc_nbt_tag_t *nbt_new_int_tag(const char *name, int32_t value) {
    mc_nbt_tag_t *tag = nbt_new_tag(MC_NBT_TAG_INT, name);
    if (!tag) return NULL;
    tag->payload.int_val = value;
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
    (void)ids;
    return mc_item_default_place_state(item_id);
}

int32_t proto_play_slot_to_state(const mc_world_ids_t *ids, const mc_slot_t *slot) {
    if (!slot || !slot->present || slot->count <= 0) return -1;
    return proto_play_item_to_state(ids, slot->item_id);
}

static int write_slot_item(mc_buf_t *b, const mc_slot_t *slot) {
    uint8_t raw[1024];
    size_t pos = 0;
    if (mc_slot_write_net(raw, sizeof(raw), &pos, slot) != 0) return -1;
    return buf_write(b, raw, pos);
}

static int send_set_slot_packet(mc_conn_t *c, uint8_t window_id, int32_t state_id, int16_t slot, const mc_slot_t *item) {
    mc_buf_t payload;
    if (buf_init(&payload, 64) != 0) return -1;
    int rc = 0;
    if (buf_w_u8(&payload, window_id) != 0 || buf_w_varint(&payload, state_id) != 0 || buf_w_u16_be(&payload, (uint16_t)slot) != 0 ||
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

static int send_window_items(mc_conn_t *c) {
    const mc_inventory_t *inv = conn_inventory_const(c);
    if (!c || !inv) return -1;
    mc_buf_t payload;
    if (buf_init(&payload, 2048) != 0) return -1;
    int rc = 0;
    if (buf_w_u8(&payload, 0) != 0 || buf_w_varint(&payload, inv->state_id) != 0 || buf_w_varint(&payload, MC_PLAYER_SLOT_COUNT) != 0) {
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
    if (container->kind == MC_CONTAINER_KIND_ENDER_CHEST) {
        return save_player_data(c);
    }
    if (!container->dirty) return 0;
    mc_world_t *world = get_world(c);
    const char *world_path = world ? mc_world_path(world) : NULL;
    if (!world_path || !*world_path) return 0;
    if (mc_container_store_save(world_path, container) != 0) {
        log_error("container save failed kind=%d pos=(%d,%d,%d) world=%s", (int)container->kind, container->x, container->y,
                  container->z, world_path);
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

static int send_close_window(mc_conn_t *c, uint8_t window_id) {
    uint8_t payload[4];
    size_t pos = 0;
    if (w_ubyte(payload, sizeof(payload), &pos, window_id) != 0) return -1;
    return conn_write_packet(c, PKT_PLAY_CLOSE_WINDOW, payload, pos, -1);
}

static int send_container_window_items(mc_conn_t *c) {
    if (!c || !c->player || !c->active_window.open || !c->active_window.container) return -1;
    mc_container_instance_t *container = c->active_window.container;
    mc_buf_t payload;
    if (buf_init(&payload, 4096) != 0) return -1;
    int total_slots = container->slot_count + 36;
    int rc = 0;
    if (buf_w_u8(&payload, (uint8_t)c->active_window.window_id) != 0 || buf_w_varint(&payload, container->state_id) != 0 ||
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
    for (int i = 27; i < 63; i++) {
        const mc_slot_t *slot = player_visible_slot_const(c->player, i);
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

static int empty_compound_nbt(uint8_t **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    mc_nbt_tag_t *root = nbt_new_tag(MC_NBT_TAG_COMPOUND, NULL);
    if (!root) return -1;
    int rc = mc_nbt_write_unnamed_root(root, out, out_len);
    mc_nbt_free(root);
    return rc;
}

static const char *container_block_entity_name(int32_t state_id) {
    state_id = mc_world_normalize_container_state_id(state_id);
    return block_entity_name_for_state(state_id);
}

static int empty_optional_nbt(uint8_t **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    uint8_t *buf = (uint8_t *)malloc(1);
    if (!buf) return -1;
    buf[0] = 0;
    if (out) *out = buf;
    if (out_len) *out_len = 1;
    return 0;
}

static int container_block_entity_chunk_nbt(int32_t state_id, int32_t x, int32_t y, int32_t z, uint8_t **out, size_t *out_len) {
    (void)state_id;
    (void)x;
    (void)y;
    (void)z;
    return empty_optional_nbt(out, out_len);
}

static int container_block_entity_update_nbt(int32_t state_id, int32_t x, int32_t y, int32_t z, uint8_t **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    const char *entity_name = container_block_entity_name(state_id);
    if (!entity_name) return empty_compound_nbt(out, out_len);

    mc_nbt_tag_t *root = nbt_new_tag(MC_NBT_TAG_COMPOUND, NULL);
    if (!root) return -1;
    root->payload.compound.length = 4;
    root->payload.compound.children = (mc_nbt_tag_t **)calloc(4, sizeof(*root->payload.compound.children));
    if (!root->payload.compound.children) {
        mc_nbt_free(root);
        return -1;
    }
    root->payload.compound.children[0] = nbt_new_string_tag("id", entity_name);
    root->payload.compound.children[1] = nbt_new_int_tag("x", x);
    root->payload.compound.children[2] = nbt_new_int_tag("y", y);
    root->payload.compound.children[3] = nbt_new_int_tag("z", z);
    for (int i = 0; i < 4; i++) {
        if (!root->payload.compound.children[i]) {
            mc_nbt_free(root);
            return -1;
        }
    }
    int rc = mc_nbt_write_unnamed_root(root, out, out_len);
    mc_nbt_free(root);
    return rc;
}

static int send_block_entity_update(mc_conn_t *c, int32_t x, int32_t y, int32_t z, int32_t state_id) {
    int32_t be_type = container_block_entity_type(state_id);
    if (!c || be_type < 0) return 0;

    uint8_t *nbt = NULL;
    size_t nbt_len = 0;
    if (container_block_entity_update_nbt(state_id, x, y, z, &nbt, &nbt_len) != 0) return -1;

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
    if (send_chunk_container_repairs(c, chunk) != 0) return -1;
    if (debug_container_pos_match(x, MC_WORLD_MIN_Y, z) || debug_containers_enabled()) {
        log_info("containers debug: authoritative chunk resend chunk=(%d,%d)", cx, cz);
    }
    return 0;
}

static void close_active_window(mc_conn_t *c, bool notify_client) {
    if (!c || !c->active_window.open) return;
    if (save_active_window(c) != 0) conn_close(c);
    uint8_t window_id = (uint8_t)c->active_window.window_id;
    close_active_window_local(c);
    if (notify_client) (void)send_close_window(c, window_id);
}

static int open_container_window(mc_conn_t *c, mc_container_instance_t *container, const char *title_key) {
    if (!c || !container) return -1;
    close_active_window(c, false);
    c->active_window.open = true;
    c->active_window.window_id = ++c->next_window_id;
    if (c->next_window_id <= 0) c->next_window_id = 1;
    c->active_window.window_type = MC_WINDOW_TYPE_GENERIC_9X3;
    c->active_window.slot_count = container->slot_count;
    c->active_window.container = container;
    if (send_open_window(c, c->active_window.window_id, c->active_window.window_type, title_key) != 0) return -1;
    return send_container_window_items(c);
}

static int sync_inventory_full(mc_conn_t *c) {
    if (send_window_items(c) != 0) return -1;
    return send_held_item_slot(c);
}

static int sync_inventory_slot(mc_conn_t *c, int16_t slot) {
    mc_inventory_t *inv = conn_inventory(c);
    if (!c || !inv) return -1;
    if (slot < 0 || slot >= MC_PLAYER_SLOT_COUNT) return -1;
    return send_set_slot_packet(c, 0, inv->state_id, slot, &inv->slots[slot]);
}

static bool state_key_is_prefix(int32_t state_id, const char *prefix) {
    const char *key = mc_block_state_key(state_id);
    return key && prefix && strncmp(key, prefix, strlen(prefix)) == 0;
}

static bool is_chest_state(int32_t state_id) {
    return state_key_is_prefix(state_id, "minecraft:chest[");
}

static bool is_trapped_chest_state(int32_t state_id) {
    return state_key_is_prefix(state_id, "minecraft:trapped_chest[");
}

static bool is_ender_chest_state(int32_t state_id) {
    return state_key_is_prefix(state_id, "minecraft:ender_chest[");
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

static const char *block_entity_name_for_state(int32_t state_id) {
    if (!block_state_has_block_entity(state_id)) return NULL;

    if ((size_t)state_id < GLOBAL_BLOCK_STATES_COUNT) {
        const mc_block_properties_t *props = &GLOBAL_BLOCK_STATES[state_id];
        if (props->block_index < GLOBAL_BLOCK_COUNT) {
            const mc_block_desc_t *desc = &GLOBAL_BLOCKS[props->block_index];
            if (desc->name && *desc->name) return desc->name;
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
    return name_buf;
}

static int container_block_entity_type(int32_t state_id) {
    const char *name = block_entity_name_for_state(state_id);
    return name ? mc_minecraft_block_entity_type_id(name) : -1;
}

static int32_t container_drop_item_id(int32_t state_id) {
    state_id = mc_world_normalize_container_state_id(state_id);
    if (is_chest_state(state_id)) return mc_minecraft_item_id("minecraft:chest");
    if (is_trapped_chest_state(state_id)) return mc_minecraft_item_id("minecraft:trapped_chest");
    if (is_ender_chest_state(state_id)) return mc_minecraft_item_id("minecraft:ender_chest");
    return -1;
}

static int spawn_drop_slot(mc_conn_t *c, double x, double y, double z, const mc_slot_t *slot) {
    if (!c || !c->server || !slot || !slot->present || slot->count <= 0) return 0;
    return net_server_spawn_item_drop(c->server, x, y, z, slot);
}

static int break_container_block(mc_conn_t *c, int32_t x, int32_t y, int32_t z, int32_t state_id) {
    if (!c) return -1;
    mc_world_t *world = get_world(c);
    const mc_world_ids_t *ids = mc_world_ids(world);
    if (!world || !ids) return -1;

    state_id = mc_world_normalize_container_state_id(state_id);
    bool is_ender = is_ender_chest_state(state_id);
    bool is_normal = is_chest_state(state_id) || is_trapped_chest_state(state_id);
    if (!is_ender && !is_normal) return 1;

    mc_container_instance_t container;
    bool have_container = false;
    if (is_normal) {
        int rc = net_server_get_open_container_snapshot(c->server, MC_CONTAINER_KIND_CHEST, x, y, z, &container);
        if (rc == 0) {
            have_container = true;
        } else if (rc == 1) {
            rc = mc_container_store_load(mc_world_path(world), MC_CONTAINER_KIND_CHEST, x, y, z, &container);
            if (rc == 0) have_container = true;
            else if (rc < 0) return -1;
        } else {
            return -1;
        }
    }

    net_server_close_container_viewers(c->server, is_ender ? MC_CONTAINER_KIND_ENDER_CHEST : MC_CONTAINER_KIND_CHEST, x, y, z);

    if (is_normal) {
        if (mc_container_store_delete(mc_world_path(world), MC_CONTAINER_KIND_CHEST, x, y, z) != 0) {
            if (have_container) mc_container_instance_clear(&container);
            return -1;
        }
    }

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
        for (int i = 0; i < MC_CONTAINER_SLOT_COUNT; i++) {
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
    if (is_chest_state(state_id)) {
        container = (mc_container_instance_t *)calloc(1, sizeof(*container));
        if (!container) return -1;
        int rc = mc_container_store_load(mc_world_path(world), MC_CONTAINER_KIND_CHEST, x, y, z, container);
        if (rc < 0) {
            free(container);
            return -1;
        }
        title_key = "container.chest";
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
    if (open_container_window(c, container, title_key) != 0) {
        mc_container_instance_clear(container);
        free(container);
        return -1;
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
    uint8_t buf[64];
    size_t pos = 0;
    if (w_varint(buf, sizeof(buf), &pos, subject->entity_id) != 0) return -1;
    if (w_f64(buf, sizeof(buf), &pos, subject->x) != 0 || w_f64(buf, sizeof(buf), &pos, subject->y) != 0 ||
        w_f64(buf, sizeof(buf), &pos, subject->z) != 0) {
        return -1;
    }
    if (w_byte(buf, sizeof(buf), &pos, (int8_t)angle_byte(subject->yaw)) != 0 ||
        w_byte(buf, sizeof(buf), &pos, (int8_t)angle_byte(subject->pitch)) != 0 ||
        w_bool(buf, sizeof(buf), &pos, true) != 0) {
        return -1;
    }
    return conn_write_packet(viewer, PKT_PLAY_ENTITY_TELEPORT, buf, pos, -1);
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
    if (debug_place_enabled()) {
        const mc_slot_t *cur = &inv->slots[slot_id];
        const char *item_name = (cur && cur->present) ? mc_minecraft_item_name(cur->item_id) : NULL;
        log_info("place debug: creative slot=%d item_id=%d item=%s count=%d", slot_id, cur->present ? cur->item_id : -1,
                 item_name ? item_name : "(empty)", cur->present ? cur->count : 0);
    }
    if (sync_inventory_slot(c, slot_id) != 0) return -1;
    return save_player_data(c);
}

static int handle_window_click(mc_conn_t *c, const mc_frame_t *frame) {
    if (!c || !frame || !c->player) return -1;
    mc_reader_t r = {frame->payload.data, frame->payload.len, 0};
    int8_t window_id = 0;
    int32_t state_id = 0;
    int16_t slot = 0;
    int8_t mouse_button = 0;
    int32_t mode = 0;
    int32_t changed_count = 0;

    if (r.pos + 1 > r.len) return -1;
    window_id = (int8_t)r.data[r.pos++];
    if (r_varint(&r, &state_id) != 0 || r_i16(&r, &slot) != 0) return -1;
    if (r.pos + 1 > r.len) return -1;
    mouse_button = (int8_t)r.data[r.pos++];
    if (r_varint(&r, &mode) != 0 || r_varint(&r, &changed_count) != 0) return -1;
    (void)state_id;
    (void)slot;
    (void)mouse_button;
    (void)mode;

    if (window_id != 0) {
        if (!c->active_window.open || window_id != c->active_window.window_id || !c->active_window.container) return 0;
        mc_container_instance_t *container = c->active_window.container;
        for (int32_t i = 0; i < changed_count; i++) {
            int16_t location = -1;
            mc_slot_t item = {0};
            if (r_i16(&r, &location) != 0 || mc_slot_read_net(r.data, r.len, &r.pos, &item) != 0) {
                mc_slot_clear(&item);
                return -1;
            }
            mc_slot_t *dst = active_container_slot(c, location);
            if (dst) {
                if (mc_slot_copy(dst, &item) != 0) {
                    mc_slot_clear(&item);
                    return -1;
                }
                container->dirty = true;
            } else {
                dst = player_visible_slot(c->player, location);
                if (dst) {
                    if (mc_slot_copy(dst, &item) != 0) {
                        mc_slot_clear(&item);
                        return -1;
                    }
                }
            }
            mc_slot_clear(&item);
        }

        mc_slot_t cursor = {0};
        if (mc_slot_read_net(r.data, r.len, &r.pos, &cursor) != 0) {
            mc_slot_clear(&cursor);
            return -1;
        }
        if (mc_slot_copy(&c->player->inventory.cursor_slot, &cursor) != 0) {
            mc_slot_clear(&cursor);
            return -1;
        }
        mc_slot_clear(&cursor);

        container->state_id++;
        c->player->inventory.state_id++;
        if (container->kind == MC_CONTAINER_KIND_ENDER_CHEST) {
            c->player->ender_state_id = container->state_id;
            for (int i = 0; i < MC_CONTAINER_SLOT_COUNT; i++) {
                if (mc_slot_copy(&c->player->ender_chest[i], &container->slots[i]) != 0) return -1;
            }
        }
        if (debug_place_enabled()) {
            log_info("place debug: container_click window=%d changed=%d state_id=%d", window_id, changed_count, container->state_id);
        }
        if (send_container_window_items(c) != 0) return -1;
        if (save_active_window(c) != 0) return -1;
        return save_player_data(c);
    }

    mc_inventory_t *inv = &c->player->inventory;
    for (int32_t i = 0; i < changed_count; i++) {
        int16_t location = -1;
        mc_slot_t item = {0};
        if (r_i16(&r, &location) != 0 || mc_slot_read_net(r.data, r.len, &r.pos, &item) != 0) {
            mc_slot_clear(&item);
            return -1;
        }
        if (location >= 0 && location < MC_PLAYER_SLOT_COUNT) {
            if (mc_slot_copy(&inv->slots[location], &item) != 0) {
                mc_slot_clear(&item);
                return -1;
            }
        }
        mc_slot_clear(&item);
    }

    mc_slot_t cursor = {0};
    if (mc_slot_read_net(r.data, r.len, &r.pos, &cursor) != 0) {
        mc_slot_clear(&cursor);
        return -1;
    }
    if (mc_slot_copy(&inv->cursor_slot, &cursor) != 0) {
        mc_slot_clear(&cursor);
        return -1;
    }
    mc_slot_clear(&cursor);

    inv->state_id++;
    if (debug_place_enabled()) {
        log_info("place debug: window_click changed=%d state_id=%d", changed_count, inv->state_id);
    }
    if (sync_inventory_full(c) != 0) return -1;
    return save_player_data(c);
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

    /* PalettedContainer network encoding uses SimpleBitStorage, which pads
     * each 64-bit cell to floor(64 / bits) values instead of compacting
     * entries across word boundaries. This differs from our internal compact
     * storage and from heightmaps. */
    values_per_long = 64u / (size_t)bits;
    if (values_per_long == 0) return -1;
    word_count = (value_count + values_per_long - 1u) / values_per_long;
    if (word_count > (size_t)INT32_MAX) return -1;

    words = (uint64_t *)calloc(word_count ? word_count : 1u, sizeof(*words));
    if (!words) return -1;

    mask = (1ULL << bits) - 1ULL;
    for (size_t i = 0; i < value_count; i++) {
        size_t word_index = i / values_per_long;
        size_t index_in_word = i - (word_index * values_per_long);
        size_t shift = index_in_word * (size_t)bits;
        words[word_index] |= (((uint64_t)values[i]) & mask) << shift;
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

static int cmp_i32(const void *a, const void *b) {
    int32_t aa = *(const int32_t *)a;
    int32_t bb = *(const int32_t *)b;
    return (aa > bb) - (aa < bb);
}

static int palette_index_of(const int32_t *palette, int pal_len, int32_t v) {
    int lo = 0;
    int hi = pal_len - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int32_t mv = palette[mid];
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

static int encode_paletted_container_network(mc_buf_t *out, const int32_t *values, int entry_count, int local_max_bits) {
    int32_t palette[4096];
    int pal_len = 0;

    if (!out || !values || entry_count <= 0 || entry_count > (int)(sizeof(palette) / sizeof(palette[0]))) return -1;

    memcpy(palette, values, (size_t)entry_count * sizeof(values[0]));
    qsort(palette, (size_t)entry_count, sizeof(palette[0]), cmp_i32);
    for (int i = 0; i < entry_count; i++) {
        if (i == 0 || palette[i] != palette[i - 1]) palette[pal_len++] = palette[i];
    }

    int bits = local_max_bits == 8 ? block_palette_bits_for_size(pal_len) : biome_palette_bits_for_size(pal_len);
    if (buf_w_u8(out, (uint8_t)bits) != 0) return -1;

    if (bits == 0) {
        return buf_w_varint(out, palette[0]);
    }

    if (bits <= local_max_bits) {
        if (buf_w_varint(out, pal_len) != 0) return -1;
        for (int i = 0; i < pal_len; i++) {
            if (buf_w_varint(out, palette[i]) != 0) return -1;
        }
    }

    uint32_t packed_vals[4096];
    for (int i = 0; i < entry_count; i++) {
        if (bits <= local_max_bits) {
            int idx = palette_index_of(palette, pal_len, values[i]);
            if (idx < 0) return -1;
            packed_vals[i] = (uint32_t)idx;
        } else {
            packed_vals[i] = (uint32_t)values[i];
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
        int32_t states[4096];
        for (int i = 0; i < 4096; i++) {
            int ly = i >> 8;
            int rem = i & 255;
            int lz = rem >> 4;
            int lx = rem & 15;
            int32_t raw_state = (int32_t)mc_paletted_container_get_block(section, lx, ly, lz);
            int32_t normalized_state = mc_world_normalize_container_state_id(raw_state);
            states[i] = normalized_state;
        }

        int non_air = 0;
        for (int i = 0; i < 4096; i++) {
            if (states[i] != ids->air) non_air++;
        }
        if (buf_w_u16_be(out, (uint16_t)non_air) != 0) return -1;
        if (buf_w_u16_be(out, 0) != 0) return -1; /* fluidCount */
        if (encode_paletted_container_network(out, states, 4096, 8) != 0) return -1;

        /* Biomes: one 4x4x4 container per section. */
        int32_t biomes[64];
        for (int i = 0; i < 64; i++) biomes[i] = g_chunk_tpl.biome_id;
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

    mc_buf_t chunkdata;
    if (buf_init(&chunkdata, 32 * 1024) != 0) return -1;
    int rc = proto_play_encode_chunkdata_for_test(world, chunk, &chunkdata);
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
    if (build_motion_blocking_heightmaps(world, chunk, &hm, &hm_len) != 0) {
        hm = NULL;
        hm_len = 0;
    }

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
                if (container_block_entity_chunk_nbt(state_id, world_x, world_y, world_z, &nbt, &nbt_len) != 0) {
                    buf_free(&block_entities);
                    free(hm);
                    buf_free(&payload);
                    buf_free(&chunkdata);
                    return -1;
                }

                uint8_t packed_xz = (uint8_t)(((lx & 0x0F) << 4) | (lz & 0x0F));
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

    const uint8_t *light = g_chunk_tpl.fullbright_light ? g_chunk_tpl.fullbright_light : g_chunk_tpl.light;
    size_t light_len = g_chunk_tpl.fullbright_light ? g_chunk_tpl.fullbright_light_len : g_chunk_tpl.light_len;

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

    rc = conn_write_packet(c, PKT_PLAY_CHUNK_DATA, payload.data, payload.len, -1);
    buf_free(&block_entities);
    free(hm);
    buf_free(&payload);
    buf_free(&chunkdata);
    return rc;
}

static int send_chunk_container_repairs(mc_conn_t *c, const mc_chunk_t *chunk) {
    if (!c || !chunk) return -1;
    for (int y_index = 0; y_index < MC_WORLD_HEIGHT; y_index++) {
        int32_t world_y = MC_WORLD_MIN_Y + y_index;
        for (int lz = 0; lz < MC_CHUNK_XZ; lz++) {
            for (int lx = 0; lx < MC_CHUNK_XZ; lx++) {
                int32_t state_id = mc_world_normalize_container_state_id((int32_t)mc_chunk_get_block(chunk, lx, world_y, lz));
                if (container_block_entity_type(state_id) < 0) continue;
                int32_t world_x = chunk->cx * MC_CHUNK_XZ + lx;
                int32_t world_z = chunk->cz * MC_CHUNK_XZ + lz;
                if (send_block_update_packet(c, world_x, world_y, world_z, state_id) != 0) return -1;
                if (send_block_entity_update(c, world_x, world_y, world_z, state_id) != 0) return -1;
                if (debug_container_pos_match(world_x, world_y, world_z)) {
                    log_info("containers debug: repair packet chunk=(%d,%d) pos=(%d,%d,%d) state=%d", chunk->cx, chunk->cz,
                             world_x, world_y, world_z, state_id);
                }
            }
        }
    }
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

    int sent = 0;
    int scans = 0;
    while (sent < CHUNKS_PER_TICK) {
        if (pending_chunks_count(c) == 0) break;
        mc_chunk_req_t req;
        if (!pending_chunks_pop(c, &req)) break;

        mc_chunk_t *chunk = mc_world_get_chunk(world, req.cx, req.cz, req.prio);
        if (!chunk) {
            if (pending_chunks_push(c, req.cx, req.cz, req.prio) != 0) return -1;
            scans++;
            if (scans >= CHUNK_SEND_SCAN_LIMIT) break;
            continue;
        }

        if (send_chunk_ready(c, world, req.cx, req.cz, chunk) != 0) return -1;
        if (send_chunk_container_repairs(c, chunk) != 0) return -1;
        if (sent_chunks_insert(c, chunk_key(req.cx, req.cz)) != 0) return -1;
        sent++;
    }

    return 0;
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

    /* Game Event: Start waiting for level chunks (13) */
    pos = 0;
    if (w_ubyte(buf, sizeof(buf), &pos, 13) != 0) return -1;
    if (w_f32(buf, sizeof(buf), &pos, 0.0f) != 0) return -1;
    if (conn_write_packet(c, PKT_PLAY_GAME_EVENT, buf, pos, -1) != 0) return -1;

    mc_world_t *world = get_world(c);
    if (!world) return -1;
    if (load_chunk_template(c) != 0) {
        return proto_send_play_disconnect(c, "{\"text\":\"Chunk template load failed\"}");
    }
    if (sent_chunks_init(c, 1024) != 0 || pending_chunks_init(c, 1024) != 0) return -1;
    sent_chunks_clear(c);
    pending_chunks_clear(c);
    c->has_center_chunk = false;
    if (rebuild_chunk_stream(c, world, 0, 0, view_distance) != 0) return -1;

    /* 26.1 default spawn packet carries LevelData.RespawnData:
     * GlobalPos(dimension + block pos) + yaw + pitch. */
    pos = 0;
    if (w_string(buf, sizeof(buf), &pos, "minecraft:overworld") != 0) return -1;
    if (w_position(buf, sizeof(buf), &pos, 0, 80, 0) != 0) return -1;
    if (w_f32(buf, sizeof(buf), &pos, 0.0f) != 0) return -1;
    if (w_f32(buf, sizeof(buf), &pos, 0.0f) != 0) return -1;
    if (conn_write_packet(c, PKT_PLAY_SET_DEFAULT_SPAWN, buf, pos, -1) != 0) return -1;

    /* Synchronize Player Position */
    if (send_sync_position(c, 0.5, 80.0, 0.5, 0.0f, 0.0f) != 0) return -1;

    if (send_player_abilities(c) != 0) return -1;
    if (sync_inventory_full(c) != 0) return -1;
    if (send_entity_event(c, ENTITY_STATUS_OP_LEVEL_4) != 0) return -1;
    if (send_commands(c) != 0) return -1;

    c->play_init_sent = true;
    c->play_ready = false;
    return 0;
}

int proto_play_handle(mc_conn_t *c, const mc_frame_t *frame, int64_t now_ms) {
    if (!c || !frame) return -1;

    if (frame->packet_id == PKT_PLAY_CONFIRM_TELEPORT) {
        int32_t teleport_id = 0;
        size_t n = 0;
        if (varint_read(frame->payload.data, frame->payload.len, &teleport_id, &n) != 0) return -1;
        if (teleport_id == c->teleport_id) {
            c->play_ready = true;
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

    if (frame->packet_id == PKT_PLAY_HELD_ITEM_SLOT_SB) {
        mc_inventory_t *inv = conn_inventory(c);
        if (!inv) return -1;
        mc_reader_t r = {frame->payload.data, frame->payload.len, 0};
        int16_t slot_id = 0;
        if (r_i16(&r, &slot_id) != 0) return -1;
        if (slot_id >= 0 && slot_id < MC_PLAYER_HOTBAR_SIZE) {
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
        if (r.pos < r.len) {
            uint8_t window_id = r.data[r.pos];
            if (c->active_window.open && c->active_window.window_id == window_id) {
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
        c->x = x;
        c->y = y;
        c->z = z;
        c->has_pos = true;
        (void)on_ground;
        return 0;
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
        c->x = x;
        c->y = y;
        c->z = z;
        c->yaw = yaw;
        c->pitch = pitch;
        c->has_pos = true;
        (void)on_ground;
        return 0;
    }

    if (frame->packet_id == PKT_PLAY_SET_PLAYER_ROT) {
        mc_reader_t r = {frame->payload.data, frame->payload.len, 0};
        float yaw = 0.0f, pitch = 0.0f;
        bool on_ground = false;
        if (r_f32(&r, &yaw) != 0) return -1;
        if (r_f32(&r, &pitch) != 0) return -1;
        if (r_bool(&r, &on_ground) != 0) return -1;
        c->yaw = yaw;
        c->pitch = pitch;
        c->has_pos = true;
        (void)on_ground;
        return 0;
    }

    if (frame->packet_id == PKT_PLAY_SET_PLAYER_ON_GROUND) {
        mc_reader_t r = {frame->payload.data, frame->payload.len, 0};
        bool on_ground = false;
        if (r_bool(&r, &on_ground) != 0) return -1;
        (void)on_ground;
        return 0;
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
        (void)seq;

        if (action == 0 || action == 2) {
            mc_world_t *world = get_world(c);
            const mc_world_ids_t *ids = mc_world_ids(world);
            if (world && ids) {
                if (debug_place_enabled()) {
                    log_info("place debug: break action=%d pos=(%d,%d,%d) face=%d", action, x, y, z, face);
                }
                int32_t state_id = -1;
                if (mc_world_get_block(world, x, y, z, &state_id) == 0) {
                    int brc = break_container_block(c, x, y, z, state_id);
                    if (brc < 0) return -1;
                    if (brc == 0) return 0;
                }
                (void)mc_world_set_block(world, x, y, z, ids->air);
            }
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
            int32_t requested_state_id = proto_play_slot_to_state(ids, held);
            int32_t normalized_state_id = mc_world_normalize_container_state_id(requested_state_id);
            int32_t authoritative_state_id = -1;
            int32_t network_state_id = -1;
            bool placed_block = false;
            if (debug_place_enabled()) {
                const char *item_name = (held && held->present) ? mc_minecraft_item_name(held->item_id) : NULL;
                log_info("place debug: use_item_on begin held_slot=%d item_id=%d item=%s target=(%d,%d,%d) face=%d placed=(%d,%d,%d) seq=%d mapped=%d requested=%d normalized=%d mapped_key=%s requested_key=%s normalized_key=%s",
                         c->player ? c->player->inventory.selected_hotbar_slot : -1,
                         held && held->present ? held->item_id : -1, item_name ? item_name : "(empty)",
                         x, y, z, face, px, py, pz, seq, mapped_state_id, requested_state_id, normalized_state_id,
                         mapped_state_id >= 0 && mc_block_state_key(mapped_state_id) ? mc_block_state_key(mapped_state_id) : "(none)",
                         requested_state_id >= 0 && mc_block_state_key(requested_state_id) ? mc_block_state_key(requested_state_id) : "(none)",
                         normalized_state_id >= 0 && mc_block_state_key(normalized_state_id) ? mc_block_state_key(normalized_state_id) : "(none)");
            }
            if (normalized_state_id >= 0) {
                if (mc_world_set_block(world, px, py, pz, normalized_state_id) == 0) {
                    placed_block = true;
                    authoritative_state_id = normalized_state_id;
                    if (mc_world_get_block(world, px, py, pz, &authoritative_state_id) != 0) {
                        authoritative_state_id = normalized_state_id;
                    }
                    network_state_id = authoritative_state_id;
                    if (send_block_update_packet(c, px, py, pz, network_state_id) != 0) return -1;
                    if (has_sequence && send_block_changed_ack_packet(c, seq) != 0) return -1;
                    if (container_block_entity_type(authoritative_state_id) >= 0) {
                        if (mc_world_flush_block(world, px, py, pz) != 0) return -1;
                        (void)send_block_entity_update(c, px, py, pz, authoritative_state_id);
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

    if (chunk_stream_tick(c) != 0) return -1;
    return 0;
}
