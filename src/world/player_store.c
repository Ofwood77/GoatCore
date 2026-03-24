#include "mc_player_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define MCP_MAGIC "MCP1"
#define MCP_VERSION 2U

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} mcp_buf_t;

static void mcp_buf_free(mcp_buf_t *b) {
    if (!b) return;
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static int mcp_buf_reserve(mcp_buf_t *b, size_t need) {
    if (!b) return -1;
    if (need <= b->cap) return 0;
    size_t cap = b->cap ? b->cap * 2 : 256;
    while (cap < need) cap *= 2;
    uint8_t *next = (uint8_t *)realloc(b->data, cap);
    if (!next) return -1;
    b->data = next;
    b->cap = cap;
    return 0;
}

static int mcp_buf_write(mcp_buf_t *b, const void *src, size_t n) {
    if (!b || !src) return -1;
    if (mcp_buf_reserve(b, b->len + n) != 0) return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

static int write_u8(mcp_buf_t *b, uint8_t v) { return mcp_buf_write(b, &v, 1); }

static int write_u32_le(mcp_buf_t *b, uint32_t v) {
    uint8_t tmp[4] = {
        (uint8_t)(v & 0xFF),
        (uint8_t)((v >> 8) & 0xFF),
        (uint8_t)((v >> 16) & 0xFF),
        (uint8_t)((v >> 24) & 0xFF),
    };
    return mcp_buf_write(b, tmp, sizeof(tmp));
}

static int write_i32_le(mcp_buf_t *b, int32_t v) { return write_u32_le(b, (uint32_t)v); }

static int read_u8(const uint8_t *buf, size_t len, size_t *pos, uint8_t *out) {
    if (!buf || !pos || !out || *pos + 1 > len) return -1;
    *out = buf[(*pos)++];
    return 0;
}

static int read_u32_le(const uint8_t *buf, size_t len, size_t *pos, uint32_t *out) {
    if (!buf || !pos || !out || *pos + 4 > len) return -1;
    uint32_t v = (uint32_t)buf[*pos] | ((uint32_t)buf[*pos + 1] << 8) | ((uint32_t)buf[*pos + 2] << 16) |
                 ((uint32_t)buf[*pos + 3] << 24);
    *pos += 4;
    *out = v;
    return 0;
}

static int read_i32_le(const uint8_t *buf, size_t len, size_t *pos, int32_t *out) {
    uint32_t u = 0;
    if (read_u32_le(buf, len, pos, &u) != 0) return -1;
    *out = (int32_t)u;
    return 0;
}

static void sanitize_username(const char *src, char *dst, size_t cap) {
    if (!dst || cap == 0) return;
    size_t pos = 0;
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
    if (pos == 0 && cap > 1) {
        dst[pos++] = 'p';
    }
    dst[pos] = '\0';
}

static int player_path(char *buf, size_t cap, const char *world_path, const mc_player_data_t *player) {
    if (!buf || !world_path || !*world_path || !player) return -1;
    char name[128];
    if (player->has_uuid) {
        snprintf(name, sizeof(name),
                 "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                 player->uuid[0], player->uuid[1], player->uuid[2], player->uuid[3], player->uuid[4], player->uuid[5],
                 player->uuid[6], player->uuid[7], player->uuid[8], player->uuid[9], player->uuid[10], player->uuid[11],
                 player->uuid[12], player->uuid[13], player->uuid[14], player->uuid[15]);
    } else {
        sanitize_username(player->username, name, sizeof(name));
    }
    int n = snprintf(buf, cap, "%s/players/%s.mcp", world_path, name);
    if (n <= 0 || (size_t)n >= cap) return -1;
    return 0;
}

static int write_slot(mcp_buf_t *b, const mc_slot_t *slot) {
    if (write_u8(b, (slot && slot->present) ? 1U : 0U) != 0) return -1;
    if (!slot || !slot->present) return 0;
    if (write_i32_le(b, slot->item_id) != 0 || write_i32_le(b, slot->count) != 0 || write_i32_le(b, slot->damage) != 0 ||
        write_i32_le(b, slot->added_component_count) != 0 || write_i32_le(b, slot->removed_component_count) != 0 ||
        write_u32_le(b, (uint32_t)slot->components_len) != 0) {
        return -1;
    }
    if (slot->components_len > 0 && mcp_buf_write(b, slot->components, slot->components_len) != 0) return -1;
    return 0;
}

static int read_slot(const uint8_t *buf, size_t len, size_t *pos, mc_slot_t *slot) {
    uint8_t present = 0;
    if (read_u8(buf, len, pos, &present) != 0) return -1;
    mc_slot_clear(slot);
    if (!present) return 0;

    uint32_t comp_len = 0;
    if (read_i32_le(buf, len, pos, &slot->item_id) != 0 || read_i32_le(buf, len, pos, &slot->count) != 0 ||
        read_i32_le(buf, len, pos, &slot->damage) != 0 || read_i32_le(buf, len, pos, &slot->added_component_count) != 0 ||
        read_i32_le(buf, len, pos, &slot->removed_component_count) != 0 || read_u32_le(buf, len, pos, &comp_len) != 0) {
        return -1;
    }
    if (*pos + comp_len > len) return -1;
    if (comp_len > 0) {
        slot->components = (uint8_t *)malloc(comp_len);
        if (!slot->components) return -1;
        memcpy(slot->components, buf + *pos, comp_len);
        slot->components_len = comp_len;
    }
    *pos += comp_len;
    slot->present = true;
    return 0;
}

int mc_player_store_save(const char *world_path, const mc_player_data_t *player) {
    if (!world_path || !*world_path || !player) return -1;
    char path[1024];
    if (player_path(path, sizeof(path), world_path, player) != 0) return -1;

    mcp_buf_t payload = {0};
    if (write_u8(&payload, player->has_uuid ? 1U : 0U) != 0) goto fail;
    if (mcp_buf_write(&payload, player->uuid, sizeof(player->uuid)) != 0) goto fail;
    uint8_t name_len = (uint8_t)strnlen(player->username, sizeof(player->username) - 1);
    if (write_u8(&payload, name_len) != 0) goto fail;
    if (name_len > 0 && mcp_buf_write(&payload, player->username, name_len) != 0) goto fail;
    if (write_i32_le(&payload, player->gamemode) != 0 ||
        write_i32_le(&payload, player->inventory.selected_hotbar_slot) != 0 ||
        write_i32_le(&payload, player->inventory.state_id) != 0) {
        goto fail;
    }
    for (size_t i = 0; i < MC_PLAYER_SLOT_COUNT; i++) {
        if (write_slot(&payload, &player->inventory.slots[i]) != 0) goto fail;
    }
    if (write_slot(&payload, &player->inventory.cursor_slot) != 0) goto fail;
    if (write_i32_le(&payload, player->ender_state_id) != 0) goto fail;
    for (size_t i = 0; i < MC_CONTAINER_SLOT_COUNT; i++) {
        if (write_slot(&payload, &player->ender_chest[i]) != 0) goto fail;
    }

    uint32_t crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, payload.data, payload.len);

    FILE *fp = fopen(path, "wb");
    if (!fp) goto fail;
    if (fwrite(MCP_MAGIC, 1, 4, fp) != 4) goto io_fail;
    uint8_t header[12] = {
        (uint8_t)(MCP_VERSION & 0xFF), (uint8_t)((MCP_VERSION >> 8) & 0xFF), (uint8_t)((MCP_VERSION >> 16) & 0xFF),
        (uint8_t)((MCP_VERSION >> 24) & 0xFF), (uint8_t)(payload.len & 0xFF), (uint8_t)((payload.len >> 8) & 0xFF),
        (uint8_t)((payload.len >> 16) & 0xFF), (uint8_t)((payload.len >> 24) & 0xFF), (uint8_t)(crc & 0xFF),
        (uint8_t)((crc >> 8) & 0xFF), (uint8_t)((crc >> 16) & 0xFF), (uint8_t)((crc >> 24) & 0xFF),
    };
    if (fwrite(header, 1, sizeof(header), fp) != sizeof(header)) goto io_fail;
    if (payload.len > 0 && fwrite(payload.data, 1, payload.len, fp) != payload.len) goto io_fail;
    fclose(fp);
    mcp_buf_free(&payload);
    return 0;

io_fail:
    fclose(fp);
fail:
    mcp_buf_free(&payload);
    return -1;
}

int mc_player_store_load(const char *world_path, const uint8_t uuid[16], bool has_uuid, const char *username, mc_player_data_t *out) {
    if (!world_path || !*world_path || !out) return 1;
    mc_player_data_t key = {0};
    key.has_uuid = has_uuid;
    if (uuid) memcpy(key.uuid, uuid, sizeof(key.uuid));
    if (username) snprintf(key.username, sizeof(key.username), "%s", username);

    char path[1024];
    if (player_path(path, sizeof(path), world_path, &key) != 0) return -1;

    FILE *fp = fopen(path, "rb");
    if (!fp) return (errno == ENOENT) ? 1 : -1;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    long fsize = ftell(fp);
    if (fsize < 16) {
        fclose(fp);
        return -1;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }
    uint8_t *file_data = (uint8_t *)malloc((size_t)fsize);
    if (!file_data) {
        fclose(fp);
        return -1;
    }
    if (fread(file_data, 1, (size_t)fsize, fp) != (size_t)fsize) {
        free(file_data);
        fclose(fp);
        return -1;
    }
    fclose(fp);

    if (memcmp(file_data, MCP_MAGIC, 4) != 0) {
        free(file_data);
        return -1;
    }
    size_t pos = 4;
    uint32_t version = 0, payload_len = 0, stored_crc = 0;
    if (read_u32_le(file_data, (size_t)fsize, &pos, &version) != 0 || read_u32_le(file_data, (size_t)fsize, &pos, &payload_len) != 0 ||
        read_u32_le(file_data, (size_t)fsize, &pos, &stored_crc) != 0) {
        free(file_data);
        return -1;
    }
    if ((version != 1U && version != MCP_VERSION) || pos + payload_len != (size_t)fsize) {
        free(file_data);
        return -1;
    }
    uint32_t crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, file_data + pos, payload_len);
    if (crc != stored_crc) {
        free(file_data);
        return -1;
    }

    mc_player_data_t tmp;
    mc_player_data_init(&tmp);
    uint8_t has_uuid_u8 = 0;
    uint8_t name_len = 0;
    if (read_u8(file_data, (size_t)fsize, &pos, &has_uuid_u8) != 0) goto fail;
    tmp.has_uuid = has_uuid_u8 ? true : false;
    if (pos + sizeof(tmp.uuid) > (size_t)fsize) goto fail;
    memcpy(tmp.uuid, file_data + pos, sizeof(tmp.uuid));
    pos += sizeof(tmp.uuid);
    if (read_u8(file_data, (size_t)fsize, &pos, &name_len) != 0) goto fail;
    if (pos + name_len > (size_t)fsize || name_len >= sizeof(tmp.username)) goto fail;
    if (name_len > 0) memcpy(tmp.username, file_data + pos, name_len);
    tmp.username[name_len] = '\0';
    pos += name_len;
    if (read_i32_le(file_data, (size_t)fsize, &pos, &tmp.gamemode) != 0 ||
        read_i32_le(file_data, (size_t)fsize, &pos, &tmp.inventory.selected_hotbar_slot) != 0 ||
        read_i32_le(file_data, (size_t)fsize, &pos, &tmp.inventory.state_id) != 0) {
        goto fail;
    }
    for (size_t i = 0; i < MC_PLAYER_SLOT_COUNT; i++) {
        if (read_slot(file_data, (size_t)fsize, &pos, &tmp.inventory.slots[i]) != 0) goto fail;
    }
    if (read_slot(file_data, (size_t)fsize, &pos, &tmp.inventory.cursor_slot) != 0) goto fail;
    if (version >= 2U) {
        if (read_i32_le(file_data, (size_t)fsize, &pos, &tmp.ender_state_id) != 0) goto fail;
        for (size_t i = 0; i < MC_CONTAINER_SLOT_COUNT; i++) {
            if (read_slot(file_data, (size_t)fsize, &pos, &tmp.ender_chest[i]) != 0) goto fail;
        }
    } else {
        tmp.ender_state_id = 1;
    }

    mc_player_data_clear(out);
    *out = tmp;
    free(file_data);
    return 0;

fail:
    mc_player_data_clear(&tmp);
    free(file_data);
    return -1;
}
