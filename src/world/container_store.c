#include "mc_container_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#define MCCONT_MAGIC "MCT1"
#define MCCONT_VERSION 1U

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} mc_cont_buf_t;

static void cont_buf_free(mc_cont_buf_t *b) {
    if (!b) return;
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static int cont_buf_reserve(mc_cont_buf_t *b, size_t need) {
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

static int cont_buf_write(mc_cont_buf_t *b, const void *src, size_t n) {
    if (!b || !src) return -1;
    if (cont_buf_reserve(b, b->len + n) != 0) return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

static int write_u8(mc_cont_buf_t *b, uint8_t v) { return cont_buf_write(b, &v, 1); }

static int write_u32_le(mc_cont_buf_t *b, uint32_t v) {
    uint8_t tmp[4] = {
        (uint8_t)(v & 0xFF),
        (uint8_t)((v >> 8) & 0xFF),
        (uint8_t)((v >> 16) & 0xFF),
        (uint8_t)((v >> 24) & 0xFF),
    };
    return cont_buf_write(b, tmp, sizeof(tmp));
}

static int write_i32_le(mc_cont_buf_t *b, int32_t v) { return write_u32_le(b, (uint32_t)v); }

static int read_u8(const uint8_t *buf, size_t len, size_t *pos, uint8_t *out) {
    if (!buf || !pos || !out || *pos + 1 > len) return -1;
    *out = buf[(*pos)++];
    return 0;
}

static int read_u32_le(const uint8_t *buf, size_t len, size_t *pos, uint32_t *out) {
    if (!buf || !pos || !out || *pos + 4 > len) return -1;
    *out = (uint32_t)buf[*pos] | ((uint32_t)buf[*pos + 1] << 8) | ((uint32_t)buf[*pos + 2] << 16) |
           ((uint32_t)buf[*pos + 3] << 24);
    *pos += 4;
    return 0;
}

static int read_i32_le(const uint8_t *buf, size_t len, size_t *pos, int32_t *out) {
    uint32_t u = 0;
    if (read_u32_le(buf, len, pos, &u) != 0) return -1;
    *out = (int32_t)u;
    return 0;
}

static int container_path(char *buf, size_t cap, const char *world_path, mc_container_kind_t kind, int32_t x, int32_t y, int32_t z) {
    if (!buf || !world_path || !*world_path) return -1;
    const char *name = (kind == MC_CONTAINER_KIND_ENDER_CHEST) ? "ender" : "chest";
    int n = snprintf(buf, cap, "%s/containers/%s.%d.%d.%d.mct", world_path, name, x, y, z);
    if (n <= 0 || (size_t)n >= cap) return -1;
    return 0;
}

static int write_slot(mc_cont_buf_t *b, const mc_slot_t *slot) {
    if (write_u8(b, (slot && slot->present) ? 1U : 0U) != 0) return -1;
    if (!slot || !slot->present) return 0;
    if (write_i32_le(b, slot->item_id) != 0 || write_i32_le(b, slot->count) != 0 || write_i32_le(b, slot->damage) != 0 ||
        write_i32_le(b, slot->added_component_count) != 0 || write_i32_le(b, slot->removed_component_count) != 0 ||
        write_u32_le(b, (uint32_t)slot->components_len) != 0) {
        return -1;
    }
    if (slot->components_len > 0 && cont_buf_write(b, slot->components, slot->components_len) != 0) return -1;
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

int mc_container_store_save(const char *world_path, const mc_container_instance_t *container) {
    if (!world_path || !*world_path || !container || container->kind == MC_CONTAINER_KIND_NONE) return -1;

    char path[1024];
    if (container_path(path, sizeof(path), world_path, container->kind, container->x, container->y, container->z) != 0) return -1;

    mc_cont_buf_t payload = {0};
    if (write_i32_le(&payload, (int32_t)container->kind) != 0 || write_i32_le(&payload, container->x) != 0 ||
        write_i32_le(&payload, container->y) != 0 || write_i32_le(&payload, container->z) != 0 ||
        write_i32_le(&payload, container->slot_count) != 0 || write_i32_le(&payload, container->state_id) != 0) {
        goto fail;
    }
    for (int i = 0; i < MC_CONTAINER_SLOT_COUNT; i++) {
        if (write_slot(&payload, &container->slots[i]) != 0) goto fail;
    }

    uint32_t crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, payload.data, payload.len);

    FILE *fp = fopen(path, "wb");
    if (!fp) goto fail;
    if (fwrite(MCCONT_MAGIC, 1, 4, fp) != 4) goto io_fail;
    uint8_t header[12] = {
        (uint8_t)(MCCONT_VERSION & 0xFF), (uint8_t)((MCCONT_VERSION >> 8) & 0xFF), (uint8_t)((MCCONT_VERSION >> 16) & 0xFF),
        (uint8_t)((MCCONT_VERSION >> 24) & 0xFF), (uint8_t)(payload.len & 0xFF), (uint8_t)((payload.len >> 8) & 0xFF),
        (uint8_t)((payload.len >> 16) & 0xFF), (uint8_t)((payload.len >> 24) & 0xFF), (uint8_t)(crc & 0xFF),
        (uint8_t)((crc >> 8) & 0xFF), (uint8_t)((crc >> 16) & 0xFF), (uint8_t)((crc >> 24) & 0xFF),
    };
    if (fwrite(header, 1, sizeof(header), fp) != sizeof(header)) goto io_fail;
    if (payload.len > 0 && fwrite(payload.data, 1, payload.len, fp) != payload.len) goto io_fail;
    fclose(fp);
    cont_buf_free(&payload);
    return 0;

io_fail:
    fclose(fp);
fail:
    cont_buf_free(&payload);
    return -1;
}

int mc_container_store_load(const char *world_path, mc_container_kind_t kind, int32_t x, int32_t y, int32_t z,
                            mc_container_instance_t *out) {
    if (!out || kind == MC_CONTAINER_KIND_NONE) return -1;
    mc_container_instance_init(out, kind, x, y, z);
    if (!world_path || !*world_path) return 1;

    char path[1024];
    if (container_path(path, sizeof(path), world_path, kind, x, y, z) != 0) return -1;

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

    if (memcmp(file_data, MCCONT_MAGIC, 4) != 0) {
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
    if (version != MCCONT_VERSION || pos + payload_len != (size_t)fsize) {
        free(file_data);
        return -1;
    }
    uint32_t crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, file_data + pos, payload_len);
    if (crc != stored_crc) {
        free(file_data);
        return -1;
    }

    int32_t file_kind = 0;
    int32_t file_x = 0, file_y = 0, file_z = 0, slot_count = 0, state_id = 0;
    if (read_i32_le(file_data, (size_t)fsize, &pos, &file_kind) != 0 || read_i32_le(file_data, (size_t)fsize, &pos, &file_x) != 0 ||
        read_i32_le(file_data, (size_t)fsize, &pos, &file_y) != 0 || read_i32_le(file_data, (size_t)fsize, &pos, &file_z) != 0 ||
        read_i32_le(file_data, (size_t)fsize, &pos, &slot_count) != 0 || read_i32_le(file_data, (size_t)fsize, &pos, &state_id) != 0) {
        free(file_data);
        return -1;
    }
    if (file_kind != (int32_t)kind || file_x != x || file_y != y || file_z != z || slot_count != MC_CONTAINER_SLOT_COUNT) {
        free(file_data);
        return -1;
    }
    out->state_id = state_id > 0 ? state_id : 1;
    for (int i = 0; i < MC_CONTAINER_SLOT_COUNT; i++) {
        if (read_slot(file_data, (size_t)fsize, &pos, &out->slots[i]) != 0) {
            free(file_data);
            mc_container_instance_clear(out);
            mc_container_instance_init(out, kind, x, y, z);
            return -1;
        }
    }

    free(file_data);
    return 0;
}

int mc_container_store_delete(const char *world_path, mc_container_kind_t kind, int32_t x, int32_t y, int32_t z) {
    if (kind == MC_CONTAINER_KIND_NONE) return -1;
    if (!world_path || !*world_path) return 0;
    char path[1024];
    if (container_path(path, sizeof(path), world_path, kind, x, y, z) != 0) return -1;
    if (unlink(path) == 0) return 0;
    if (errno == ENOENT) return 0;
    return -1;
}
