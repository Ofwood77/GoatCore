#include "mc_world.h"
#include "mc_chunk_store.h"
#include "mc_container_store.h"
#include "mc_inventory.h"
#include "mc_player_store.h"
#include "mc_protocol.h"
#include "mc_anvil.h"
#include "mc_nbt.h"
#include "mc_packed.h"
#include "block_entity_store.h"
#include "mc_net.h"
#include "mc_server.h"
#include "generated_minecraft_ids.h"
#include "generated_registries.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

static int fail(const char *msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
}

static int32_t require_item_id(const char *name) {
    int32_t id = mc_minecraft_item_id(name);
    if (id < 0) fprintf(stderr, "FAIL: missing item id for %s\n", name);
    return id;
}

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
    (void)slot;
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

static mc_nbt_tag_t *nbt_new(mc_nbt_type_t type, const char *name) {
    mc_nbt_tag_t *t = (mc_nbt_tag_t *)calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->type = type;
    if (name) {
        t->name = strdup(name);
        if (!t->name) {
            free(t);
            return NULL;
        }
    }
    return t;
}

static mc_nbt_tag_t *nbt_new_string(const char *name, const char *value) {
    mc_nbt_tag_t *t = nbt_new(MC_NBT_TAG_STRING, name);
    if (!t) return NULL;
    t->payload.string_val = strdup(value ? value : "");
    if (!t->payload.string_val) {
        mc_nbt_free(t);
        return NULL;
    }
    return t;
}

static mc_nbt_tag_t *palette_entry_name_only(const char *name_value) {
    mc_nbt_tag_t *entry = nbt_new(MC_NBT_TAG_COMPOUND, NULL);
    if (!entry) return NULL;
    entry->payload.compound.length = 1;
    entry->payload.compound.children = (mc_nbt_tag_t **)calloc(1, sizeof(*entry->payload.compound.children));
    if (!entry->payload.compound.children) {
        mc_nbt_free(entry);
        return NULL;
    }
    entry->payload.compound.children[0] = nbt_new_string("Name", name_value);
    if (!entry->payload.compound.children[0]) {
        mc_nbt_free(entry);
        return NULL;
    }
    return entry;
}

static int wait_block_equals(mc_world_t *w, int32_t x, int32_t y, int32_t z, int32_t expected) {
    int32_t got = -1;
    for (int i = 0; i < 400; i++) {
        mc_world_tick(w, 0);
        if (mc_world_get_block(w, x, y, z, &got) != 0) return -1;
        if (got == expected) return 0;
        struct timespec ts = {0};
        ts.tv_nsec = 1000000;
        nanosleep(&ts, NULL);
    }
    return -1;
}

static int build_padded_section_data_4096(uint32_t *values, int bits, int64_t **out_longs, int32_t *out_len) {
    if (out_longs) *out_longs = NULL;
    if (out_len) *out_len = 0;
    if (!values || !out_longs || !out_len) return -1;
    if (bits <= 0 || bits > 32) return -1;

    int values_per_long = 64 / bits;
    if (values_per_long <= 0) return -1;
    int long_count = (4096 + values_per_long - 1) / values_per_long;
    int64_t *longs = (int64_t *)calloc((size_t)long_count, sizeof(*longs));
    if (!longs) return -1;

    uint64_t mask = (bits >= 64) ? UINT64_MAX : ((1ULL << bits) - 1ULL);
    for (int i = 0; i < 4096; i++) {
        int li = i / values_per_long;
        int shift = (i % values_per_long) * bits;
        uint64_t word = (uint64_t)longs[li];
        word |= ((uint64_t)values[i] & mask) << shift;
        longs[li] = (int64_t)word;
    }

    *out_longs = longs;
    *out_len = long_count;
    return 0;
}

static int wait_chunk_saved(mc_world_t *w, int32_t cx, int32_t cz) {
    for (int i = 0; i < 400; i++) {
        mc_world_tick(w, 0);
        mc_chunk_t *chunk = mc_world_get_chunk(w, cx, cz, 0);
        if (chunk && !chunk->dirty) return 0;
        struct timespec ts = {0};
        ts.tv_nsec = 1000000;
        nanosleep(&ts, NULL);
    }
    return -1;
}

static int wait_chunk_loaded(mc_world_t *w, int32_t cx, int32_t cz) {
    for (int i = 0; i < 400; i++) {
        mc_world_tick(w, 0);
        mc_chunk_t *chunk = mc_world_get_chunk(w, cx, cz, 0);
        if (chunk) return 0;
        struct timespec ts = {0};
        ts.tv_nsec = 1000000;
        nanosleep(&ts, NULL);
    }
    return -1;
}

static int verify_chunkstore_block_equals(const char *world_path,
                                          int32_t cx,
                                          int32_t cz,
                                          int32_t x,
                                          int32_t y,
                                          int32_t z,
                                          int32_t expected) {
    mc_chunk_t chunk = {0};
    if (mc_chunk_store_read(world_path, cx, cz, &chunk) != 0) return -1;
    if (y < MC_WORLD_MIN_Y || y >= MC_WORLD_MIN_Y + MC_WORLD_HEIGHT) {
        mc_chunk_destroy(&chunk);
        return -1;
    }
    int lx = x - (cx * MC_CHUNK_XZ);
    int lz = z - (cz * MC_CHUNK_XZ);
    if (lx < 0 || lx >= MC_CHUNK_XZ || lz < 0 || lz >= MC_CHUNK_XZ) {
        mc_chunk_destroy(&chunk);
        return -1;
    }
    int rc = mc_chunk_get_block(&chunk, lx, y, lz) == (mc_global_state_id_t)expected ? 0 : -1;
    mc_chunk_destroy(&chunk);
    return rc;
}

static int chunk_store_path(char *buf, size_t cap, const char *world_path, int32_t cx, int32_t cz) {
    int n = snprintf(buf, cap, "%s/chunks/c.%d.%d.mcc", world_path, cx, cz);
    if (n <= 0 || (size_t)n >= cap) return -1;
    return 0;
}

static int read_u16_be_local(const uint8_t *buf, size_t len, size_t *pos, uint16_t *out) {
    if (!buf || !pos || !out || *pos + 2 > len) return -1;
    *out = (uint16_t)(((uint16_t)buf[*pos] << 8) | (uint16_t)buf[*pos + 1]);
    *pos += 2;
    return 0;
}

static int read_u8_local(const uint8_t *buf, size_t len, size_t *pos, uint8_t *out) {
    if (!buf || !pos || !out || *pos + 1 > len) return -1;
    *out = buf[*pos];
    *pos += 1;
    return 0;
}

static int read_varint_local(const uint8_t *buf, size_t len, size_t *pos, int32_t *out) {
    size_t n = 0;
    if (!buf || !pos || !out || *pos >= len) return -1;
    if (varint_read(buf + *pos, len - *pos, out, &n) != 0) return -1;
    *pos += n;
    return 0;
}

static int word_count_local(int entry_count, int bits) {
    int values_per_long;
    if (bits <= 0) return 0;
    values_per_long = 64 / bits;
    if (values_per_long <= 0) return -1;
    return (entry_count + values_per_long - 1) / values_per_long;
}

static int chunkdata_section_contains_state(const mc_buf_t *buf, int section_index, int32_t target_state_id, bool *out_found) {
    if (out_found) *out_found = false;
    if (!buf || !out_found || section_index < 0 || section_index >= MC_WORLD_SECTION_COUNT) return -1;

    size_t pos = 0;
    for (int sec = 0; sec < MC_WORLD_SECTION_COUNT; sec++) {
        uint16_t non_air = 0;
        uint16_t fluid_count = 0;
        uint8_t bits = 0;
        if (read_u16_be_local(buf->data, buf->len, &pos, &non_air) != 0) return -1;
        if (read_u16_be_local(buf->data, buf->len, &pos, &fluid_count) != 0) return -1;
        if (read_u8_local(buf->data, buf->len, &pos, &bits) != 0) return -1;
        (void)fluid_count;

        if (bits == 0) {
            int32_t single = -1;
            if (read_varint_local(buf->data, buf->len, &pos, &single) != 0) return -1;
            if (sec == section_index) *out_found = (single == target_state_id);
        } else {
            bool found = false;
            if (bits <= 8) {
                int32_t palette_len = 0;
                if (read_varint_local(buf->data, buf->len, &pos, &palette_len) != 0) return -1;
                for (int i = 0; i < palette_len; i++) {
                    int32_t state_id = -1;
                    if (read_varint_local(buf->data, buf->len, &pos, &state_id) != 0) return -1;
                    if (sec == section_index && state_id == target_state_id) found = true;
                }
            }
            int words = word_count_local(4096, bits);
            if (words < 0 || pos + (size_t)words * 8u > buf->len) return -1;
            pos += (size_t)words * 8u;
            if (sec == section_index) *out_found = found;
        }

        uint8_t biome_bits = 0;
        int32_t biome_value = 0;
        if (read_u8_local(buf->data, buf->len, &pos, &biome_bits) != 0) return -1;
        if (biome_bits == 0) {
            if (read_varint_local(buf->data, buf->len, &pos, &biome_value) != 0) return -1;
        } else {
            int32_t biome_palette_len = 0;
            if (read_varint_local(buf->data, buf->len, &pos, &biome_palette_len) != 0) return -1;
            for (int i = 0; i < biome_palette_len; i++) {
                if (read_varint_local(buf->data, buf->len, &pos, &biome_value) != 0) return -1;
            }
            int words = word_count_local(64, biome_bits);
            if (words < 0 || pos + (size_t)words * 8u > buf->len) return -1;
            pos += (size_t)words * 8u;
        }
    }
    return 0;
}

static int write_uniform_anvil_chunk(const char *region_path, int8_t sec_y, const char *name_value) {
    mc_nbt_tag_t *data_version = nbt_new(MC_NBT_TAG_INT, "DataVersion");
    mc_nbt_tag_t *xpos = nbt_new(MC_NBT_TAG_INT, "xPos");
    mc_nbt_tag_t *zpos = nbt_new(MC_NBT_TAG_INT, "zPos");
    mc_nbt_tag_t *sections = nbt_new(MC_NBT_TAG_LIST, "sections");
    if (!data_version || !xpos || !zpos || !sections) return -1;
    data_version->payload.int_val = 4785;
    xpos->payload.int_val = 0;
    zpos->payload.int_val = 0;
    sections->payload.list.elem_type = MC_NBT_TAG_COMPOUND;
    sections->payload.list.length = 1;
    sections->payload.list.items = (mc_nbt_tag_t **)calloc(1, sizeof(*sections->payload.list.items));
    if (!sections->payload.list.items) {
        mc_nbt_free(xpos);
        mc_nbt_free(zpos);
        mc_nbt_free(sections);
        return -1;
    }

    mc_nbt_tag_t *sec = nbt_new(MC_NBT_TAG_COMPOUND, NULL);
    mc_nbt_tag_t *y = nbt_new(MC_NBT_TAG_BYTE, "Y");
    mc_nbt_tag_t *bs = nbt_new(MC_NBT_TAG_COMPOUND, "block_states");
    mc_nbt_tag_t *palette = nbt_new(MC_NBT_TAG_LIST, "palette");
    if (!sec || !y || !bs || !palette) {
        mc_nbt_free(sec);
        mc_nbt_free(y);
        mc_nbt_free(bs);
        mc_nbt_free(palette);
        mc_nbt_free(data_version);
        mc_nbt_free(xpos);
        mc_nbt_free(zpos);
        mc_nbt_free(sections);
        return -1;
    }

    y->payload.byte_val = sec_y;
    palette->payload.list.elem_type = MC_NBT_TAG_COMPOUND;
    palette->payload.list.length = 1;
    palette->payload.list.items = (mc_nbt_tag_t **)calloc(1, sizeof(*palette->payload.list.items));
    if (!palette->payload.list.items) {
        mc_nbt_free(sec);
        mc_nbt_free(y);
        mc_nbt_free(bs);
        mc_nbt_free(palette);
        mc_nbt_free(data_version);
        mc_nbt_free(xpos);
        mc_nbt_free(zpos);
        mc_nbt_free(sections);
        return -1;
    }
    palette->payload.list.items[0] = palette_entry_name_only(name_value);
    if (!palette->payload.list.items[0]) {
        mc_nbt_free(sec);
        mc_nbt_free(y);
        mc_nbt_free(bs);
        mc_nbt_free(palette);
        mc_nbt_free(data_version);
        mc_nbt_free(xpos);
        mc_nbt_free(zpos);
        mc_nbt_free(sections);
        return -1;
    }

    bs->payload.compound.length = 1;
    bs->payload.compound.children = (mc_nbt_tag_t **)calloc(1, sizeof(*bs->payload.compound.children));
    if (!bs->payload.compound.children) {
        mc_nbt_free(sec);
        mc_nbt_free(y);
        mc_nbt_free(bs);
        mc_nbt_free(palette);
        mc_nbt_free(data_version);
        mc_nbt_free(xpos);
        mc_nbt_free(zpos);
        mc_nbt_free(sections);
        return -1;
    }
    bs->payload.compound.children[0] = palette;

    sec->payload.compound.length = 2;
    sec->payload.compound.children = (mc_nbt_tag_t **)calloc(2, sizeof(*sec->payload.compound.children));
    if (!sec->payload.compound.children) {
        mc_nbt_free(sec);
        mc_nbt_free(y);
        mc_nbt_free(bs);
        mc_nbt_free(data_version);
        mc_nbt_free(xpos);
        mc_nbt_free(zpos);
        mc_nbt_free(sections);
        return -1;
    }
    sec->payload.compound.children[0] = y;
    sec->payload.compound.children[1] = bs;
    sections->payload.list.items[0] = sec;

    mc_nbt_tag_t *root = nbt_new(MC_NBT_TAG_COMPOUND, NULL);
    if (!root) {
        mc_nbt_free(data_version);
        mc_nbt_free(xpos);
        mc_nbt_free(zpos);
        mc_nbt_free(sections);
        return -1;
    }
    root->payload.compound.length = 4;
    root->payload.compound.children = (mc_nbt_tag_t **)calloc(4, sizeof(*root->payload.compound.children));
    if (!root->payload.compound.children) {
        mc_nbt_free(root);
        mc_nbt_free(data_version);
        mc_nbt_free(xpos);
        mc_nbt_free(zpos);
        mc_nbt_free(sections);
        return -1;
    }
    root->payload.compound.children[0] = data_version;
    root->payload.compound.children[1] = xpos;
    root->payload.compound.children[2] = zpos;
    root->payload.compound.children[3] = sections;

    uint8_t *nbt_bytes = NULL;
    size_t nbt_len = 0;
    int rc = mc_nbt_write_named_root(root, &nbt_bytes, &nbt_len);
    if (rc == 0) rc = mc_anvil_write_chunk_nbt(region_path, 0, 0, nbt_bytes, nbt_len);
    free(nbt_bytes);
    mc_nbt_free(root);
    return rc;
}

static int test_anvil_block_entity_import(const char *root) {
    char world[1024];
    char region[1024];
    mc_nbt_tag_t *data_version = NULL;
    mc_nbt_tag_t *xpos = NULL;
    mc_nbt_tag_t *zpos = NULL;
    mc_nbt_tag_t *sections = NULL;
    mc_nbt_tag_t *sec = NULL;
    mc_nbt_tag_t *y = NULL;
    mc_nbt_tag_t *bs = NULL;
    mc_nbt_tag_t *palette = NULL;
    mc_nbt_tag_t *block_entities = NULL;
    mc_nbt_tag_t *be = NULL;
    mc_nbt_tag_t *root_tag = NULL;
    uint8_t *nbt_bytes = NULL;
    size_t nbt_len = 0;
    mc_chunk_t chunk = {0};
    mc_block_entity_store_t store;
    mc_arena_t arena;
    mc_block_entity_t *found = NULL;
    int rc = 1;

    if (snprintf(world, sizeof(world), "%s/world_block_entities", root) >= (int)sizeof(world)) {
        return fail("block entity world path");
    }
    if (snprintf(region, sizeof(region), "%s/region/r.0.0.mca", world) >= (int)sizeof(region)) {
        return fail("block entity region path");
    }

    mc_world_t *w = mc_world_create(world, 0);
    if (!w) return fail("mc_world_create block entities");
    mc_world_destroy(w);

    data_version = nbt_new(MC_NBT_TAG_INT, "DataVersion");
    xpos = nbt_new(MC_NBT_TAG_INT, "xPos");
    zpos = nbt_new(MC_NBT_TAG_INT, "zPos");
    sections = nbt_new(MC_NBT_TAG_LIST, "sections");
    sec = nbt_new(MC_NBT_TAG_COMPOUND, NULL);
    y = nbt_new(MC_NBT_TAG_BYTE, "Y");
    bs = nbt_new(MC_NBT_TAG_COMPOUND, "block_states");
    palette = nbt_new(MC_NBT_TAG_LIST, "palette");
    block_entities = nbt_new(MC_NBT_TAG_LIST, "block_entities");
    be = nbt_new(MC_NBT_TAG_COMPOUND, NULL);
    root_tag = nbt_new(MC_NBT_TAG_COMPOUND, NULL);
    if (!data_version || !xpos || !zpos || !sections || !sec || !y || !bs || !palette || !block_entities || !be || !root_tag) {
        rc = fail("block entity nbt alloc");
        goto cleanup;
    }

    data_version->payload.int_val = 4785;
    xpos->payload.int_val = 0;
    zpos->payload.int_val = 0;
    y->payload.byte_val = 8;

    palette->payload.list.elem_type = MC_NBT_TAG_COMPOUND;
    palette->payload.list.length = 1;
    palette->payload.list.items = (mc_nbt_tag_t **)calloc(1, sizeof(*palette->payload.list.items));
    sections->payload.list.elem_type = MC_NBT_TAG_COMPOUND;
    sections->payload.list.length = 1;
    sections->payload.list.items = (mc_nbt_tag_t **)calloc(1, sizeof(*sections->payload.list.items));
    bs->payload.compound.length = 1;
    bs->payload.compound.children = (mc_nbt_tag_t **)calloc(1, sizeof(*bs->payload.compound.children));
    sec->payload.compound.length = 2;
    sec->payload.compound.children = (mc_nbt_tag_t **)calloc(2, sizeof(*sec->payload.compound.children));
    block_entities->payload.list.elem_type = MC_NBT_TAG_COMPOUND;
    block_entities->payload.list.length = 1;
    block_entities->payload.list.items = (mc_nbt_tag_t **)calloc(1, sizeof(*block_entities->payload.list.items));
    be->payload.compound.length = 4;
    be->payload.compound.children = (mc_nbt_tag_t **)calloc(4, sizeof(*be->payload.compound.children));
    root_tag->payload.compound.length = 5;
    root_tag->payload.compound.children = (mc_nbt_tag_t **)calloc(5, sizeof(*root_tag->payload.compound.children));
    if (!palette->payload.list.items || !sections->payload.list.items || !bs->payload.compound.children || !sec->payload.compound.children ||
        !block_entities->payload.list.items || !be->payload.compound.children || !root_tag->payload.compound.children) {
        rc = fail("block entity nbt children alloc");
        goto cleanup;
    }

    bs->payload.compound.children[0] = palette;
    sec->payload.compound.children[0] = y;
    sec->payload.compound.children[1] = bs;
    sections->payload.list.items[0] = sec;
    block_entities->payload.list.items[0] = be;
    root_tag->payload.compound.children[0] = data_version;
    root_tag->payload.compound.children[1] = xpos;
    root_tag->payload.compound.children[2] = zpos;
    root_tag->payload.compound.children[3] = sections;
    root_tag->payload.compound.children[4] = block_entities;

    palette->payload.list.items[0] = palette_entry_name_only("minecraft:chest");
    be->payload.compound.children[0] = nbt_new_string("id", "minecraft:chest");
    be->payload.compound.children[1] = nbt_new(MC_NBT_TAG_INT, "x");
    be->payload.compound.children[2] = nbt_new(MC_NBT_TAG_INT, "y");
    be->payload.compound.children[3] = nbt_new(MC_NBT_TAG_INT, "z");
    if (!palette->payload.list.items[0] || !be->payload.compound.children[0] || !be->payload.compound.children[1] ||
        !be->payload.compound.children[2] || !be->payload.compound.children[3]) {
        rc = fail("block entity child tag alloc");
        goto cleanup;
    }
    be->payload.compound.children[1]->payload.int_val = 0;
    be->payload.compound.children[2]->payload.int_val = 64;
    be->payload.compound.children[3]->payload.int_val = 0;

    if (mc_nbt_write_named_root(root_tag, &nbt_bytes, &nbt_len) != 0) {
        rc = fail("block entity write named root");
        goto cleanup;
    }
    if (mc_anvil_write_chunk_nbt(region, 0, 0, nbt_bytes, nbt_len) != 0) {
        rc = fail("block entity write anvil");
        goto cleanup;
    }

    mc_be_store_init(&store);
    if (mc_chunk_init(&chunk, 0, 0, 0u) != 0) {
        mc_be_store_destroy(&store);
        rc = fail("block entity chunk init");
        goto cleanup;
    }
    if (mc_arena_init(&arena, 2u * 1024u * 1024u) != 0) {
        mc_chunk_destroy(&chunk);
        mc_be_store_destroy(&store);
        rc = fail("block entity arena init");
        goto cleanup;
    }

    if (mc_anvil_load_chunk(region, 0, 0, &chunk, &store, &arena) != 0) {
        mc_arena_destroy(&arena);
        mc_chunk_destroy(&chunk);
        mc_be_store_destroy(&store);
        rc = fail("block entity load chunk");
        goto cleanup;
    }

    found = mc_be_store_get(&store, (mc_pos_t){0, 64, 0});
    mc_arena_destroy(&arena);
    mc_chunk_destroy(&chunk);
    if (!found || found->type != MC_BLOCK_ENTITY_CHEST) {
        mc_be_store_destroy(&store);
        rc = fail("block entity import mismatch");
        goto cleanup;
    }
    mc_be_store_destroy(&store);
    rc = 0;

cleanup:
    free(nbt_bytes);
    mc_nbt_free(root_tag);
    return rc;
}

static int test_slot_roundtrip(void) {
    int32_t dirt_id = require_item_id("minecraft:dirt");
    if (dirt_id < 0) return 1;
    mc_slot_t in = {0};
    if (mc_slot_set_simple(&in, dirt_id, 64) != 0) return fail("slot_set_simple");
    uint8_t buf[64];
    size_t pos = 0;
    if (mc_slot_write_net(buf, sizeof(buf), &pos, &in) != 0) {
        mc_slot_clear(&in);
        return fail("slot_write_net");
    }
    size_t read_pos = 0;
    mc_slot_t out = {0};
    if (mc_slot_read_net(buf, pos, &read_pos, &out) != 0) {
        mc_slot_clear(&in);
        return fail("slot_read_net");
    }
    int ok = out.present && out.item_id == dirt_id && out.count == 64;
    mc_slot_clear(&in);
    mc_slot_clear(&out);
    return ok ? 0 : fail("slot roundtrip mismatch");
}

static int test_player_store_roundtrip(const char *root) {
    int32_t stone_id = require_item_id("minecraft:stone");
    int32_t dirt_id = require_item_id("minecraft:dirt");
    int32_t water_bucket_id = require_item_id("minecraft:water_bucket");
    int32_t chest_id = require_item_id("minecraft:chest");
    if (stone_id < 0 || dirt_id < 0 || water_bucket_id < 0 || chest_id < 0) return 1;
    char world[1024];
    if (snprintf(world, sizeof(world), "%s/world_players", root) >= (int)sizeof(world)) return fail("player world path");
    mc_world_t *w = mc_world_create(world, 0);
    if (!w) return fail("mc_world_create players");
    mc_world_destroy(w);

    mc_player_data_t player;
    mc_player_data_init(&player);
    snprintf(player.username, sizeof(player.username), "%s", "InventoryTester");
    player.gamemode = 1;
    player.inventory.selected_hotbar_slot = 3;
    player.inventory.state_id = 42;
    if (mc_slot_set_simple(&player.inventory.slots[36], stone_id, 64) != 0 ||
        mc_slot_set_simple(&player.inventory.slots[37], dirt_id, 12) != 0 ||
        mc_slot_set_simple(&player.inventory.cursor_slot, water_bucket_id, 1) != 0 ||
        mc_slot_set_simple(&player.ender_chest[0], chest_id, 4) != 0) {
        mc_player_data_clear(&player);
        return fail("player slot set");
    }
    player.ender_state_id = 9;
    if (mc_player_store_save(world, &player) != 0) {
        mc_player_data_clear(&player);
        return fail("player store save");
    }

    mc_player_data_t loaded;
    mc_player_data_init(&loaded);
    int rc = mc_player_store_load(world, NULL, false, "InventoryTester", &loaded);
    if (rc != 0) {
        mc_player_data_clear(&player);
        mc_player_data_clear(&loaded);
        return fail("player store load");
    }
    int ok = loaded.gamemode == 1 && loaded.inventory.selected_hotbar_slot == 3 && loaded.inventory.state_id == 42 &&
             loaded.inventory.slots[36].present && loaded.inventory.slots[36].item_id == stone_id &&
             loaded.inventory.slots[37].present && loaded.inventory.slots[37].item_id == dirt_id &&
             loaded.inventory.cursor_slot.present && loaded.inventory.cursor_slot.item_id == water_bucket_id &&
             loaded.ender_state_id == 9 &&
             loaded.ender_chest[0].present && loaded.ender_chest[0].item_id == chest_id;
    mc_player_data_clear(&player);
    mc_player_data_clear(&loaded);
    return ok ? 0 : fail("player store roundtrip mismatch");
}

static int test_container_store_roundtrip(const char *root) {
    int32_t chest_id = require_item_id("minecraft:chest");
    int32_t ender_chest_id = require_item_id("minecraft:ender_chest");
    if (chest_id < 0 || ender_chest_id < 0) return 1;
    char world[1024];
    if (snprintf(world, sizeof(world), "%s/world_containers", root) >= (int)sizeof(world)) return fail("container world path");
    mc_world_t *w = mc_world_create(world, 0);
    if (!w) return fail("mc_world_create containers");
    mc_world_destroy(w);

    mc_container_instance_t chest;
    mc_container_instance_init(&chest, MC_CONTAINER_KIND_CHEST, 1, 80, 2);
    chest.state_id = 7;
    if (mc_slot_set_simple(&chest.slots[0], chest_id, 12) != 0 || mc_slot_set_simple(&chest.slots[26], ender_chest_id, 1) != 0) {
        mc_container_instance_clear(&chest);
        return fail("container slot set");
    }
    if (mc_container_store_save(world, &chest) != 0) {
        mc_container_instance_clear(&chest);
        return fail("container save");
    }

    mc_container_instance_t loaded;
    int rc = mc_container_store_load(world, MC_CONTAINER_KIND_CHEST, 1, 80, 2, &loaded);
    mc_container_instance_clear(&chest);
    if (rc != 0) {
        mc_container_instance_clear(&loaded);
        return fail("container load");
    }
    int ok = loaded.kind == MC_CONTAINER_KIND_CHEST && loaded.state_id == 7 &&
             loaded.slots[0].present && loaded.slots[0].item_id == chest_id &&
             loaded.slots[26].present && loaded.slots[26].item_id == ender_chest_id;
    mc_container_instance_clear(&loaded);
    return ok ? 0 : fail("container store roundtrip mismatch");
}

static int test_container_store_delete(const char *root) {
    int32_t chest_id = require_item_id("minecraft:chest");
    if (chest_id < 0) return 1;
    char world[1024];
    if (snprintf(world, sizeof(world), "%s/world_containers_delete", root) >= (int)sizeof(world)) return fail("container delete path");
    mc_world_t *w = mc_world_create(world, 0);
    if (!w) return fail("mc_world_create containers delete");
    mc_world_destroy(w);

    mc_container_instance_t chest;
    mc_container_instance_init(&chest, MC_CONTAINER_KIND_CHEST, 4, 70, 5);
    chest.state_id = 7;
    if (mc_slot_set_simple(&chest.slots[3], chest_id, 1) != 0) {
        mc_container_instance_clear(&chest);
        return fail("container delete slot set");
    }
    if (mc_container_store_save(world, &chest) != 0) {
        mc_container_instance_clear(&chest);
        return fail("container delete save");
    }
    mc_container_instance_clear(&chest);

    if (mc_container_store_delete(world, MC_CONTAINER_KIND_CHEST, 4, 70, 5) != 0) {
        return fail("container delete unlink");
    }

    mc_container_instance_t loaded;
    int rc = mc_container_store_load(world, MC_CONTAINER_KIND_CHEST, 4, 70, 5, &loaded);
    mc_container_instance_clear(&loaded);
    return (rc == 1) ? 0 : fail("container delete load expected absent");
}

static int test_container_state_truth(const char *root) {
    char world[1024];
    if (snprintf(world, sizeof(world), "%s/world_container_state", root) >= (int)sizeof(world)) return fail("container state path");
    mc_world_t *w = mc_world_create(world, 0);
    if (!w) return fail("mc_world_create container state");

    const mc_world_ids_t *ids = mc_world_ids(w);
    if (!ids) {
        mc_world_destroy(w);
        return fail("container state ids");
    }

    int32_t chest_true = mc_block_state_id("minecraft:chest[facing=north,type=single,waterlogged=true]", -1);
    int32_t chest_false = mc_block_state_id("minecraft:chest[facing=north,type=single,waterlogged=false]", -1);
    int32_t ender_true = mc_block_state_id("minecraft:ender_chest[facing=north,waterlogged=true]", -1);
    int32_t ender_false = mc_block_state_id("minecraft:ender_chest[facing=north,waterlogged=false]", -1);
    if (chest_true < 0 || chest_false < 0 || ender_true < 0 || ender_false < 0) {
        mc_world_destroy(w);
        return fail("container state ids missing");
    }

    if (mc_world_normalize_container_state_id(chest_true) != chest_true ||
        mc_world_normalize_container_state_id(ender_true) != ender_true) {
        mc_world_destroy(w);
        return fail("shared container normalize helper pass-through");
    }

    if (wait_chunk_loaded(w, 0, 0) != 0) {
        mc_world_destroy(w);
        return fail("container state initial chunk");
    }

    if (mc_world_set_block(w, 1, 64, 1, chest_true) != 0 || mc_world_set_block(w, 2, 64, 2, ender_true) != 0) {
        mc_world_destroy(w);
        return fail("container state set_block");
    }

    int32_t got = -1;
    if (mc_world_get_block(w, 1, 64, 1, &got) != 0 || got != chest_true) {
        mc_world_destroy(w);
        return fail("container state chest preserved in ram");
    }
    if (mc_world_get_block(w, 2, 64, 2, &got) != 0 || got != ender_true) {
        mc_world_destroy(w);
        return fail("container state ender preserved in ram");
    }

    if (wait_chunk_saved(w, 0, 0) != 0) {
        mc_world_destroy(w);
        return fail("container state save");
    }
    if (verify_chunkstore_block_equals(world, 0, 0, 1, 64, 1, chest_true) != 0 ||
        verify_chunkstore_block_equals(world, 0, 0, 2, 64, 2, ender_true) != 0) {
        mc_world_destroy(w);
        return fail("container state saved preserved");
    }

    mc_chunk_t raw = {0};
    if (mc_chunk_init(&raw, 0, 0, (mc_global_state_id_t)ids->air) != 0) {
        mc_world_destroy(w);
        return fail("container state raw chunk init");
    }
    raw.loaded = true;
    if (mc_chunk_set_block(&raw, 1, 64, 1, (mc_global_state_id_t)chest_true) != 0) {
        mc_chunk_destroy(&raw);
        mc_world_destroy(w);
        return fail("container state raw set");
    }
    if (mc_chunk_store_write(world, &raw) != 0) {
        mc_chunk_destroy(&raw);
        mc_world_destroy(w);
        return fail("container state legacy write");
    }
    mc_chunk_destroy(&raw);
    mc_world_destroy(w);

    mc_world_t *w2 = mc_world_create(world, 0);
    if (!w2) return fail("mc_world_create container state reload");
    if (wait_block_equals(w2, 1, 64, 1, chest_true) != 0) {
        mc_world_destroy(w2);
        return fail("container state preserved on load");
    }
    if (verify_chunkstore_block_equals(world, 0, 0, 1, 64, 1, chest_true) != 0) {
        mc_world_destroy(w2);
        return fail("container state rewrite preserved");
    }

    mc_chunk_t chunk = {0};
    if (mc_chunk_init(&chunk, 0, 0, (mc_global_state_id_t)ids->air) != 0) {
        mc_world_destroy(w2);
        return fail("chunkdata test chunk init");
    }
    chunk.loaded = true;
    if (mc_chunk_set_block(&chunk, 1, 64, 1, (mc_global_state_id_t)chest_true) != 0) {
        mc_chunk_destroy(&chunk);
        mc_world_destroy(w2);
        return fail("chunkdata test set");
    }

    mc_buf_t encoded;
    if (buf_init(&encoded, 4096) != 0) {
        mc_chunk_destroy(&chunk);
        mc_world_destroy(w2);
        return fail("chunkdata test buf_init");
    }
    if (proto_play_encode_chunkdata_for_test(w2, &chunk, &encoded) != 0) {
        mc_chunk_destroy(&chunk);
        buf_free(&encoded);
        mc_world_destroy(w2);
        return fail("chunkdata encode test");
    }
    bool found_false = false;
    bool found_true = false;
    int section_index = (64 - MC_WORLD_MIN_Y) / 16;
    if (chunkdata_section_contains_state(&encoded, section_index, chest_false, &found_false) != 0 ||
        chunkdata_section_contains_state(&encoded, section_index, chest_true, &found_true) != 0) {
        mc_chunk_destroy(&chunk);
        buf_free(&encoded);
        mc_world_destroy(w2);
        return fail("chunkdata parse test");
    }
    mc_chunk_destroy(&chunk);
    buf_free(&encoded);
    mc_world_destroy(w2);
    if (found_false || !found_true) return fail("chunkdata contains wrong chest state");
    return 0;
}

int main(void) {
    char tmpl[] = "/tmp/mc_world_roundtripXXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) return fail("mkdtemp");

    if (test_slot_roundtrip() != 0) return 1;
    if (test_player_store_roundtrip(dir) != 0) return 1;
    if (test_container_store_roundtrip(dir) != 0) return 1;
    if (test_container_store_delete(dir) != 0) return 1;
    if (test_container_state_truth(dir) != 0) return 1;
    if (test_anvil_block_entity_import(dir) != 0) return 1;

    /* Use a non-existing nested path to validate mkdir_p(world_path + /region). */
    char world[1024];
    snprintf(world, sizeof(world), "%s/world", dir);

    mc_world_t *w = mc_world_create(world, 0);
    if (!w) return fail("mc_world_create");

    const mc_world_ids_t *ids = mc_world_ids(w);
    if (!ids || ids->redstone_block < 0 || ids->water_level[0] < 0 || ids->grass_block_snowy_false < 0) {
        mc_world_destroy(w);
        return fail("missing ids (redstone/water/grass)");
    }
    int32_t id_air = ids->air;
    int32_t id_stone = ids->stone;
    int32_t id_dirt = ids->dirt;
    int32_t id_water0 = ids->water_level[0];
    int32_t id_grass = ids->grass_block_snowy_false;
    int32_t id_redstone0 = ids->redstone_block;
    int32_t item_dirt = require_item_id("minecraft:dirt");
    int32_t item_chest = require_item_id("minecraft:chest");
    int32_t item_ender_chest = require_item_id("minecraft:ender_chest");
    int32_t item_furnace = require_item_id("minecraft:furnace");
    int32_t item_oak_log = require_item_id("minecraft:oak_log");
    int32_t item_redstone_block = require_item_id("minecraft:redstone_block");
    int32_t item_water_bucket = require_item_id("minecraft:water_bucket");
    int32_t item_lava_bucket = require_item_id("minecraft:lava_bucket");
    if (item_dirt < 0 || item_chest < 0 || item_ender_chest < 0 || item_furnace < 0 || item_oak_log < 0 ||
        item_redstone_block < 0 || item_water_bucket < 0 || item_lava_bucket < 0) {
        mc_world_destroy(w);
        return 1;
    }

    if (proto_play_item_to_state(ids, item_dirt) != id_dirt) {
        mc_world_destroy(w);
        return fail("item->state dirt mapping");
    }
    if (proto_play_item_to_state(ids, item_chest) != mc_block_state_id("minecraft:chest[facing=north,type=single,waterlogged=false]", -1)) {
        mc_world_destroy(w);
        return fail("item->state chest mapping");
    }
    if (proto_play_item_to_state(ids, item_ender_chest) != mc_block_state_id("minecraft:ender_chest[facing=north,waterlogged=false]", -1)) {
        mc_world_destroy(w);
        return fail("item->state ender chest mapping");
    }
    if (proto_play_item_to_state(ids, item_oak_log) != mc_block_state_id("minecraft:oak_log[axis=y]", -1)) {
        mc_world_destroy(w);
        return fail("item->state oak_log default mapping");
    }
    if (proto_play_item_to_state(ids, item_redstone_block) != id_redstone0) {
        mc_world_destroy(w);
        return fail("item->state redstone_block mapping");
    }
    if (proto_play_item_to_state(ids, item_water_bucket) != id_water0) {
        mc_world_destroy(w);
        return fail("item->state water_bucket mapping");
    }
    if (proto_play_item_to_state(ids, item_lava_bucket) != ids->lava_level[0]) {
        mc_world_destroy(w);
        return fail("item->state lava_bucket mapping");
    }

    mc_slot_t placement_slot = {0};
    if (mc_slot_set_simple(&placement_slot, item_chest, 1) != 0) {
        mc_world_destroy(w);
        return fail("setup chest placement slot");
    }
    if (proto_play_resolve_placement_state(ids, &placement_slot, 1, 90.0f, 0.0f) !=
        mc_block_state_id("minecraft:chest[facing=east,type=single,waterlogged=false]", -1)) {
        mc_world_destroy(w);
        return fail("placement chest facing player");
    }
    mc_slot_clear(&placement_slot);

    if (mc_slot_set_simple(&placement_slot, item_furnace, 1) != 0) {
        mc_world_destroy(w);
        return fail("setup furnace placement slot");
    }
    if (proto_play_resolve_placement_state(ids, &placement_slot, 1, 90.0f, 0.0f) !=
        mc_block_state_id("minecraft:furnace[facing=east,lit=false]", -1)) {
        mc_world_destroy(w);
        return fail("placement furnace facing player");
    }
    mc_slot_clear(&placement_slot);

    if (mc_slot_set_simple(&placement_slot, item_oak_log, 1) != 0) {
        mc_world_destroy(w);
        return fail("setup oak_log placement slot");
    }
    if (proto_play_resolve_placement_state(ids, &placement_slot, 5, 0.0f, 0.0f) !=
            mc_block_state_id("minecraft:oak_log[axis=x]", -1) ||
        proto_play_resolve_placement_state(ids, &placement_slot, 1, 0.0f, 0.0f) !=
            mc_block_state_id("minecraft:oak_log[axis=y]", -1) ||
        proto_play_resolve_placement_state(ids, &placement_slot, 2, 0.0f, 0.0f) !=
            mc_block_state_id("minecraft:oak_log[axis=z]", -1)) {
        mc_world_destroy(w);
        return fail("placement oak_log natural axis");
    }
    mc_slot_clear(&placement_slot);

    if (wait_chunk_loaded(w, 0, 0) != 0) {
        mc_world_destroy(w);
        return fail("initial chunk load");
    }

    /* Runtime-realistic path: break terrain block, wait save, evict, reload, verify from custom snapshot and RAM. */
    if (mc_world_set_block(w, 0, 60, 0, id_air) != 0) {
        mc_world_destroy(w);
        return fail("set_block terrain air");
    }
    if (wait_chunk_saved(w, 0, 0) != 0) {
        mc_world_destroy(w);
        return fail("chunk save before evict");
    }
    if (verify_chunkstore_block_equals(world, 0, 0, 0, 60, 0, id_air) != 0) {
        mc_world_destroy(w);
        return fail("chunkstore verify air before evict");
    }
    if (mc_world_evict_outside(w, NULL, 0, 4) == 0) {
        mc_world_destroy(w);
        return fail("evict chunk");
    }
    if (wait_block_equals(w, 0, 60, 0, id_air) != 0) {
        mc_world_destroy(w);
        return fail("runtime evict/reload air mismatch");
    }

    /* A: set water */
    if (mc_world_set_block(w, 0, 80, 0, id_water0) != 0) {
        mc_world_destroy(w);
        return fail("set_block water");
    }

    /* B: restore terrain and break again for destroy/recreate path */
    if (mc_world_set_block(w, 0, 60, 0, id_stone) != 0) {
        mc_world_destroy(w);
        return fail("set_block stone");
    }
    if (mc_world_set_block(w, 0, 60, 0, id_air) != 0) {
        mc_world_destroy(w);
        return fail("set_block air");
    }

    /* C: block with properties */
    if (mc_world_set_block(w, 0, 81, 0, id_grass) != 0) {
        mc_world_destroy(w);
        return fail("set_block grass");
    }

    if (wait_chunk_saved(w, 0, 0) != 0) {
        mc_world_destroy(w);
        return fail("chunk save before recreate");
    }
    if (verify_chunkstore_block_equals(world, 0, 0, 0, 60, 0, id_air) != 0) {
        mc_world_destroy(w);
        return fail("chunkstore verify air before recreate");
    }
    mc_world_destroy(w);

    mc_world_t *w2 = mc_world_create(world, 0);
    if (!w2) return fail("mc_world_create (2)");

    if (wait_block_equals(w2, 0, 80, 0, id_water0) != 0) {
        mc_world_destroy(w2);
        return fail("roundtrip water mismatch");
    }
    if (wait_block_equals(w2, 0, 60, 0, id_air) != 0) {
        mc_world_destroy(w2);
        return fail("roundtrip air mismatch");
    }
    if (wait_block_equals(w2, 0, 81, 0, id_grass) != 0) {
        mc_world_destroy(w2);
        return fail("roundtrip grass mismatch");
    }

    mc_world_destroy(w2);

    /* Priority: custom snapshot wins over base Anvil. */
    char world3[1024];
    if (snprintf(world3, sizeof(world3), "%s/world_priority", dir) >= (int)sizeof(world3)) return fail("world3 path");
    mc_world_t *wp = mc_world_create(world3, 0);
    if (!wp) return fail("mc_world_create priority");
    char region3[1024];
    if (snprintf(region3, sizeof(region3), "%s/region/r.0.0.mca", world3) >= (int)sizeof(region3)) return fail("region3 path");
    if (write_uniform_anvil_chunk(region3, 3, "minecraft:stone") != 0) {
        mc_world_destroy(wp);
        return fail("write priority anvil");
    }
    mc_chunk_t snapshot = {0};
    if (mc_chunk_init(&snapshot, 0, 0, (mc_global_state_id_t)id_stone) != 0) {
        mc_world_destroy(wp);
        return fail("priority snapshot init");
    }
    snapshot.loaded = true;
    if (mc_chunk_set_block(&snapshot, 0, 60, 0, (mc_global_state_id_t)id_air) != 0) {
        mc_chunk_destroy(&snapshot);
        mc_world_destroy(wp);
        return fail("priority snapshot set");
    }
    if (mc_chunk_store_write(world3, &snapshot) != 0) {
        mc_chunk_destroy(&snapshot);
        mc_world_destroy(wp);
        return fail("write priority chunkstore");
    }
    mc_chunk_destroy(&snapshot);
    mc_world_destroy(wp);

    mc_world_t *wp2 = mc_world_create(world3, 0);
    if (!wp2) return fail("mc_world_create priority reload");
    if (wait_block_equals(wp2, 0, 60, 0, id_air) != 0) {
        mc_world_destroy(wp2);
        return fail("chunkstore priority over anvil mismatch");
    }
    mc_world_destroy(wp2);

    /* Corruption fallback: bad snapshot should fall back to Anvil, then rewrite clean snapshot. */
    char world4[1024];
    if (snprintf(world4, sizeof(world4), "%s/world_corrupt", dir) >= (int)sizeof(world4)) return fail("world4 path");
    mc_world_t *wc = mc_world_create(world4, 0);
    if (!wc) return fail("mc_world_create corrupt");
    char region4[1024];
    if (snprintf(region4, sizeof(region4), "%s/region/r.0.0.mca", world4) >= (int)sizeof(region4)) return fail("region4 path");
    if (write_uniform_anvil_chunk(region4, 5, "minecraft:redstone_block") != 0) {
        mc_world_destroy(wc);
        return fail("write corrupt anvil");
    }
    char store4[1024];
    if (chunk_store_path(store4, sizeof(store4), world4, 0, 0) != 0) {
        mc_world_destroy(wc);
        return fail("store4 path");
    }
    FILE *bad = fopen(store4, "wb");
    if (!bad) {
        mc_world_destroy(wc);
        return fail("open corrupt snapshot");
    }
    (void)fwrite("bad!", 1, 4, bad);
    fclose(bad);
    mc_world_destroy(wc);

    mc_world_t *wc2 = mc_world_create(world4, 0);
    if (!wc2) return fail("mc_world_create corrupt reload");
    if (wait_block_equals(wc2, 0, 80, 0, id_redstone0) != 0) {
        mc_world_destroy(wc2);
        return fail("corrupt snapshot fallback to anvil mismatch");
    }
    if (wait_chunk_saved(wc2, 0, 0) != 0) {
        mc_world_destroy(wc2);
        return fail("corrupt snapshot rewrite save");
    }
    if (verify_chunkstore_block_equals(world4, 0, 0, 0, 80, 0, id_redstone0) != 0) {
        mc_world_destroy(wc2);
        return fail("corrupt snapshot rewrite verify");
    }
    mc_world_destroy(wc2);

    /* Retro-compat: write a chunk with PADDED block_states.data and ensure loader can read it. */
    char world5[1024];
    if (snprintf(world5, sizeof(world5), "%s/world_padded", dir) >= (int)sizeof(world5)) return fail("world5 path");
    mc_world_t *w3 = mc_world_create(world5, 0);
    if (!w3) return fail("mc_world_create padded");
    const mc_world_ids_t *ids3 = mc_world_ids(w3);
    if (!ids3 || ids3->redstone_block < 0) {
        mc_world_destroy(w3);
        return fail("missing ids padded");
    }
    int32_t id_redstone = ids3->redstone_block;
    mc_nbt_tag_t *data_version = nbt_new(MC_NBT_TAG_INT, "DataVersion");
    mc_nbt_tag_t *xpos = nbt_new(MC_NBT_TAG_INT, "xPos");
    mc_nbt_tag_t *zpos = nbt_new(MC_NBT_TAG_INT, "zPos");
    mc_nbt_tag_t *sections = nbt_new(MC_NBT_TAG_LIST, "sections");
    if (!data_version || !xpos || !zpos || !sections) return fail("nbt alloc");
    data_version->payload.int_val = 4785;
    xpos->payload.int_val = 0;
    zpos->payload.int_val = 0;
    sections->payload.list.elem_type = MC_NBT_TAG_COMPOUND;
    sections->payload.list.length = 1;
    sections->payload.list.items = (mc_nbt_tag_t **)calloc(1, sizeof(*sections->payload.list.items));
    if (!sections->payload.list.items) return fail("nbt alloc sections");

    mc_nbt_tag_t *sec = nbt_new(MC_NBT_TAG_COMPOUND, NULL);
    mc_nbt_tag_t *y = nbt_new(MC_NBT_TAG_BYTE, "Y");
    mc_nbt_tag_t *bs = nbt_new(MC_NBT_TAG_COMPOUND, "block_states");
    mc_nbt_tag_t *palette = nbt_new(MC_NBT_TAG_LIST, "palette");
    if (!sec || !y || !bs || !palette) return fail("nbt alloc sec");

    y->payload.byte_val = 5; /* section containing y=80 for MIN_Y=-64 */
    palette->payload.list.elem_type = MC_NBT_TAG_COMPOUND;
    palette->payload.list.length = 2;
    palette->payload.list.items = (mc_nbt_tag_t **)calloc(2, sizeof(*palette->payload.list.items));
    if (!palette->payload.list.items) return fail("nbt alloc palette");
    palette->payload.list.items[0] = palette_entry_name_only("minecraft:air");
    palette->payload.list.items[1] = palette_entry_name_only("minecraft:redstone_block");
    if (!palette->payload.list.items[0] || !palette->payload.list.items[1]) return fail("nbt palette entry");

    uint32_t vals[4096];
    memset(vals, 0, sizeof(vals));
    vals[0] = 1; /* (lx=0,lz=0,ly=0) */
    int bits = 4;
    int64_t *padded_longs = NULL;
    int32_t padded_len = 0;
    if (build_padded_section_data_4096(vals, bits, &padded_longs, &padded_len) != 0) return fail("build padded data");

    mc_nbt_tag_t *data = nbt_new(MC_NBT_TAG_LONG_ARRAY, "data");
    if (!data) return fail("nbt data");
    data->payload.long_array.length = padded_len;
    data->payload.long_array.data = padded_longs;

    bs->payload.compound.length = 2;
    bs->payload.compound.children = (mc_nbt_tag_t **)calloc(2, sizeof(*bs->payload.compound.children));
    if (!bs->payload.compound.children) return fail("nbt bs children");
    bs->payload.compound.children[0] = palette;
    bs->payload.compound.children[1] = data;

    sec->payload.compound.length = 2;
    sec->payload.compound.children = (mc_nbt_tag_t **)calloc(2, sizeof(*sec->payload.compound.children));
    if (!sec->payload.compound.children) return fail("nbt sec children");
    sec->payload.compound.children[0] = y;
    sec->payload.compound.children[1] = bs;
    sections->payload.list.items[0] = sec;

    mc_nbt_tag_t *root = nbt_new(MC_NBT_TAG_COMPOUND, NULL);
    if (!root) return fail("nbt root");
    root->payload.compound.length = 4;
    root->payload.compound.children = (mc_nbt_tag_t **)calloc(4, sizeof(*root->payload.compound.children));
    if (!root->payload.compound.children) return fail("nbt root children");
    root->payload.compound.children[0] = data_version;
    root->payload.compound.children[1] = xpos;
    root->payload.compound.children[2] = zpos;
    root->payload.compound.children[3] = sections;

    uint8_t *nbt_bytes = NULL;
    size_t nbt_len = 0;
    if (mc_nbt_write_named_root(root, &nbt_bytes, &nbt_len) != 0) return fail("nbt write");

    char region[1024];
    if (snprintf(region, sizeof(region), "%s/region/r.0.0.mca", world5) >= (int)sizeof(region)) return fail("region path");
    if (mc_anvil_write_chunk_nbt(region, 0, 0, nbt_bytes, nbt_len) != 0) return fail("anvil write padded");

    free(nbt_bytes);
    mc_nbt_free(root);
    mc_world_destroy(w3);

    mc_world_t *w5 = mc_world_create(world5, 0);
    if (!w5) return fail("mc_world_create padded reload");
    if (wait_block_equals(w5, 0, 80, 0, id_redstone) != 0) {
        mc_world_destroy(w5);
        return fail("padded decode mismatch");
    }
    mc_world_destroy(w5);

    return 0;
}
