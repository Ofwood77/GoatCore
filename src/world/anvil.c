#include "mc_anvil.h"

#include "block_registry.h"
#include "generated_minecraft_ids.h"
#include "generated_registries.h"
#include "mc_packed.h"
#include "mc_world.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <zlib.h>

#define MC_ANVIL_SECTOR_BYTES 4096
#define MC_ANVIL_HEADER_BYTES 8192
#define MC_ANVIL_OFFSETS_BYTES 4096
#define MC_ANVIL_MAX_CHUNK_NBT (64u * 1024u * 1024u)

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int32_t positive_mod_i32(int32_t value, int32_t divisor) {
    int32_t rem = value % divisor;
    return rem < 0 ? rem + divisor : rem;
}

static int ceil_log2_u32(uint32_t v) {
    int bits = 0;

    if (v <= 1) return 0;
    v--;
    while (v) {
        bits++;
        v >>= 1;
    }
    return bits;
}

static int cmp_nbt_tag_ptr_name(const void *a, const void *b) {
    const mc_nbt_tag_t *ta = *(const mc_nbt_tag_t *const *)a;
    const mc_nbt_tag_t *tb = *(const mc_nbt_tag_t *const *)b;

    if (!ta || !tb) return (ta != NULL) - (tb != NULL);
    if (!ta->name || !tb->name) return (ta->name != NULL) - (tb->name != NULL);
    return strcmp(ta->name, tb->name);
}

static int nbt_num_to_i32_local(const mc_nbt_tag_t *tag, int32_t *out) {
    if (!tag || !out) return -1;
    switch (tag->type) {
        case MC_NBT_TAG_BYTE: *out = tag->payload.byte_val; return 0;
        case MC_NBT_TAG_SHORT: *out = tag->payload.short_val; return 0;
        case MC_NBT_TAG_INT: *out = tag->payload.int_val; return 0;
        case MC_NBT_TAG_LONG: *out = (int32_t)tag->payload.long_val; return 0;
        default: return -1;
    }
}

static char *canonicalize_palette_key_in_arena(const char *name, const mc_nbt_tag_t *props, mc_arena_t *arena) {
    mc_nbt_tag_t **sorted;
    size_t cap;
    size_t pos;
    int32_t nprops;
    bool first = true;
    char *out;

    if (!name || !*name || !arena) return NULL;
    if (!props || props->type != MC_NBT_TAG_COMPOUND || props->payload.compound.length <= 0) {
        out = (char *)mc_arena_alloc(arena, strlen(name) + 1);
        if (!out) return NULL;
        strcpy(out, name);
        return out;
    }

    nprops = props->payload.compound.length;
    if ((size_t)nprops > SIZE_MAX / sizeof(*sorted)) return NULL;
    sorted = (mc_nbt_tag_t **)mc_arena_alloc(arena, (size_t)nprops * sizeof(*sorted));
    if (!sorted) return NULL;
    for (int32_t i = 0; i < nprops; i++) sorted[i] = props->payload.compound.children[i];
    qsort(sorted, (size_t)nprops, sizeof(*sorted), cmp_nbt_tag_ptr_name);

    cap = strlen(name) + 3;
    for (int32_t i = 0; i < nprops; i++) {
        mc_nbt_tag_t *t = sorted[i];
        if (!t || !t->name || t->type != MC_NBT_TAG_STRING || !t->payload.string_val) continue;
        cap += strlen(t->name) + strlen(t->payload.string_val) + 2;
    }

    out = (char *)mc_arena_alloc(arena, cap);
    if (!out) return NULL;

    pos = 0;
    memcpy(out + pos, name, strlen(name));
    pos += strlen(name);
    out[pos++] = '[';
    for (int32_t i = 0; i < nprops; i++) {
        mc_nbt_tag_t *t = sorted[i];
        size_t kn;
        size_t vn;

        if (!t || !t->name || t->type != MC_NBT_TAG_STRING || !t->payload.string_val) continue;
        if (!first) out[pos++] = ',';
        first = false;
        kn = strlen(t->name);
        vn = strlen(t->payload.string_val);
        memcpy(out + pos, t->name, kn);
        pos += kn;
        out[pos++] = '=';
        memcpy(out + pos, t->payload.string_val, vn);
        pos += vn;
    }
    out[pos++] = ']';
    out[pos] = '\0';
    return out;
}

static uint32_t decode_paletted_index(const int64_t *longs, int32_t longs_len, int bits_per_block, int index, bool *out_compact) {
    size_t expected_compact;
    size_t bit_index;
    size_t word_index;
    uint64_t mask;
    uint64_t value;
    int shift;
    int spill;

    if (out_compact) *out_compact = true;
    if (!longs || longs_len <= 0 || bits_per_block <= 0 || index < 0 || index >= 4096) return 0;

    expected_compact = mc_packed_compact_long_count(4096, bits_per_block);
    if ((size_t)longs_len != expected_compact) {
        int values_per_long = 64 / bits_per_block;
        int li = index / values_per_long;
        int local_shift = (index % values_per_long) * bits_per_block;

        if (out_compact) *out_compact = false;
        if (li < 0 || li >= longs_len) return 0;
        mask = (bits_per_block >= 64) ? UINT64_MAX : ((1ULL << bits_per_block) - 1ULL);
        return (uint32_t)(((uint64_t)longs[li] >> local_shift) & mask);
    }

    /* Vanilla 1.16+ packs values as one contiguous bitstream across the whole long[]
     * with no per-word padding. We therefore locate the first bit of entry N at
     * bit_index = N * bits_per_block, read the low bits from the current 64-bit word,
     * and, if the value crosses the boundary, pull the remaining high bits from the
     * next word. */
    bit_index = (size_t)index * (size_t)bits_per_block;
    word_index = bit_index >> 6;
    shift = (int)(bit_index & 63u);
    value = ((uint64_t)longs[word_index]) >> shift;
    spill = shift + bits_per_block - 64;
    if (spill > 0 && word_index + 1 < (size_t)longs_len) {
        value |= ((uint64_t)longs[word_index + 1]) << (bits_per_block - (uint8_t)spill);
    }
    mask = (bits_per_block >= 64) ? UINT64_MAX : ((1ULL << bits_per_block) - 1ULL);
    return (uint32_t)(value & mask);
}

static mc_block_entity_type_t block_entity_type_from_id(const char *id) {
    if (!id) return MC_BLOCK_ENTITY_NONE;
    if (strcmp(id, "minecraft:chest") == 0 || strcmp(id, "minecraft:trapped_chest") == 0 ||
        strcmp(id, "minecraft:ender_chest") == 0) {
        return strcmp(id, "minecraft:ender_chest") == 0 ? MC_BLOCK_ENTITY_ENDER_CHEST : MC_BLOCK_ENTITY_CHEST;
    }
    if (strcmp(id, "minecraft:barrel") == 0) return MC_BLOCK_ENTITY_BARREL;
    if (strcmp(id, "minecraft:dropper") == 0 || strcmp(id, "minecraft:dispenser") == 0 ||
        strcmp(id, "minecraft:hopper") == 0) {
        return MC_BLOCK_ENTITY_DROPPER;
    }
    if (strcmp(id, "minecraft:furnace") == 0) return MC_BLOCK_ENTITY_FURNACE;
    if (strcmp(id, "minecraft:smoker") == 0) return MC_BLOCK_ENTITY_SMOKER;
    if (strcmp(id, "minecraft:blast_furnace") == 0) return MC_BLOCK_ENTITY_BLAST_FURNACE;
    if (strstr(id, "shulker_box")) return MC_BLOCK_ENTITY_SHULKER_BOX;
    if (strcmp(id, "minecraft:sign") == 0 || strcmp(id, "minecraft:hanging_sign") == 0 ||
        strcmp(id, "minecraft:wall_sign") == 0 || strcmp(id, "minecraft:wall_hanging_sign") == 0) {
        return MC_BLOCK_ENTITY_SIGN;
    }
    return MC_BLOCK_ENTITY_NONE;
}

static int inflate_buffer(const uint8_t *src, size_t src_len, uint8_t **out, size_t *out_len) {
    if (!src || !out || !out_len) return -1;

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.next_in = (Bytef *)src;
    strm.avail_in = (uInt)src_len;

    if (inflateInit2(&strm, 15 + 32) != Z_OK) {
        return -1;
    }

    size_t cap = src_len ? (src_len * 4) : 4096;
    if (cap < 4096) cap = 4096;
    if (cap > MC_ANVIL_MAX_CHUNK_NBT) cap = MC_ANVIL_MAX_CHUNK_NBT;

    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) {
        inflateEnd(&strm);
        return -1;
    }

    int ret = Z_OK;
    while (ret != Z_STREAM_END) {
        if (strm.total_out >= cap) {
            if (cap >= MC_ANVIL_MAX_CHUNK_NBT) {
                free(buf);
                inflateEnd(&strm);
                return -1;
            }
            size_t new_cap = cap * 2;
            if (new_cap < cap) {
                free(buf);
                inflateEnd(&strm);
                return -1;
            }
            if (new_cap > MC_ANVIL_MAX_CHUNK_NBT) new_cap = MC_ANVIL_MAX_CHUNK_NBT;
            uint8_t *next = (uint8_t *)realloc(buf, new_cap);
            if (!next) {
                free(buf);
                inflateEnd(&strm);
                return -1;
            }
            buf = next;
            cap = new_cap;
        }

        strm.next_out = buf + strm.total_out;
        strm.avail_out = (uInt)(cap - (size_t)strm.total_out);
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) break;
        if (ret != Z_OK) {
            free(buf);
            inflateEnd(&strm);
            return -1;
        }
    }

    inflateEnd(&strm);

    size_t n = (size_t)strm.total_out;
    uint8_t *shrunk = (uint8_t *)realloc(buf, n ? n : 1);
    if (shrunk) buf = shrunk;

    *out = buf;
    *out_len = n;
    return 0;
}

int mc_anvil_read_chunk_nbt(const char *region_path, int local_x, int local_z, uint8_t **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!region_path || !out || !out_len) return -1;
    if (local_x < 0 || local_x > 31 || local_z < 0 || local_z > 31) return -1;

    FILE *f = fopen(region_path, "rb");
    if (!f) {
        if (errno == ENOENT) return 1;
        return -1;
    }

    uint8_t header[MC_ANVIL_HEADER_BYTES];
    size_t n = fread(header, 1, sizeof(header), f);
    if (n != sizeof(header)) {
        fclose(f);
        return -1;
    }

    int idx = local_x + local_z * 32;
    const uint8_t *ent = header + (idx * 4);
    uint32_t entry = read_be32(ent);
    uint32_t sector_off = entry >> 8;
    uint8_t sector_count = (uint8_t)(entry & 0xFF);
    if (sector_off == 0 || sector_count == 0) {
        fclose(f);
        return 1;
    }

    uint64_t chunk_off_bytes = (uint64_t)sector_off * MC_ANVIL_SECTOR_BYTES;
    if (fseek(f, (long)chunk_off_bytes, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }

    uint8_t chunk_header[5];
    if (fread(chunk_header, 1, sizeof(chunk_header), f) != sizeof(chunk_header)) {
        fclose(f);
        return -1;
    }

    uint32_t chunk_len = read_be32(chunk_header);
    uint8_t compression_type = chunk_header[4];
    if (chunk_len < 1) {
        fclose(f);
        return -1;
    }

    size_t max_payload = (size_t)sector_count * MC_ANVIL_SECTOR_BYTES;
    if (chunk_len + 4 > max_payload) {
        fclose(f);
        return -1;
    }

    size_t comp_len = (size_t)chunk_len - 1;
    uint8_t *comp = NULL;
    if (comp_len > 0) {
        comp = (uint8_t *)malloc(comp_len);
        if (!comp) {
            fclose(f);
            return -1;
        }
        if (fread(comp, 1, comp_len, f) != comp_len) {
            free(comp);
            fclose(f);
            return -1;
        }
    }
    fclose(f);

    if (compression_type == 3) {
        *out = comp;
        *out_len = comp_len;
        return 0;
    }

    if (compression_type != 1 && compression_type != 2) {
        free(comp);
        return -1;
    }

    uint8_t *inflated = NULL;
    size_t inflated_len = 0;
    int rc = inflate_buffer(comp, comp_len, &inflated, &inflated_len);
    free(comp);
    if (rc != 0) return -1;
    *out = inflated;
    *out_len = inflated_len;
    return 0;
}

int mc_anvil_decode_chunk_nbt(const uint8_t *nbt_buf,
                              size_t nbt_len,
                              int chunk_x,
                              int chunk_z,
                              mc_chunk_t *out_chunk,
                              mc_block_entity_store_t *be_store,
                              mc_arena_t *temp_arena) {
    size_t bytes_read = 0;
    mc_nbt_tag_t *root = NULL;
    const mc_nbt_tag_t *sections = NULL;
    const mc_nbt_tag_t *block_entities = NULL;
    int rc = -1;

    if (!nbt_buf || nbt_len == 0 || !out_chunk || !be_store || !temp_arena) return -1;

    if (mc_nbt_read_named_root_arena(nbt_buf, nbt_len, temp_arena, &root, &bytes_read) != 0 || !root) {
        rc = -1;
        goto cleanup;
    }
    if (!mc_anvil_validate_chunk(root)) {
        rc = -1;
        goto cleanup;
    }

    {
        const mc_nbt_tag_t *xpos_tag = mc_nbt_compound_get(root, "xPos");
        const mc_nbt_tag_t *zpos_tag = mc_nbt_compound_get(root, "zPos");
        if (!xpos_tag || xpos_tag->type != MC_NBT_TAG_INT || !zpos_tag || zpos_tag->type != MC_NBT_TAG_INT) {
            rc = -1;
            goto cleanup;
        }
        if (xpos_tag->payload.int_val != chunk_x || zpos_tag->payload.int_val != chunk_z) {
            errno = EIO;
            rc = -1;
            goto cleanup;
        }
    }

    sections = mc_nbt_compound_get(root, "sections");
    if (!sections || sections->type != MC_NBT_TAG_LIST) {
        rc = 0;
        goto cleanup;
    }

    for (int32_t si = 0; si < sections->payload.list.length; si++) {
        mc_nbt_tag_t *sec = sections->payload.list.items ? sections->payload.list.items[si] : NULL;
        const mc_nbt_tag_t *y_tag;
        const mc_nbt_tag_t *block_states;
        const mc_nbt_tag_t *palette;
        const mc_nbt_tag_t *data;
        mc_global_state_id_t *palette_map;
        int32_t palette_size;
        int32_t sec_y;
        int32_t sec_idx;
        int bits_per_block;
        const int64_t *longs = NULL;
        int32_t longs_len = 0;

        if (!sec || sec->type != MC_NBT_TAG_COMPOUND) continue;

        y_tag = mc_nbt_compound_get(sec, "Y");
        if (!y_tag) y_tag = mc_nbt_compound_get(sec, "yPos");
        if (!y_tag) continue;
        if (y_tag->type == MC_NBT_TAG_BYTE) sec_y = (int32_t)y_tag->payload.byte_val;
        else if (y_tag->type == MC_NBT_TAG_INT) sec_y = y_tag->payload.int_val;
        else continue;

        sec_idx = ((sec_y * 16) + 64) / 16;
        if (sec_idx < 0 || sec_idx >= MC_WORLD_SECTION_COUNT) continue;

        block_states = mc_nbt_compound_get(sec, "block_states");
        if (!block_states || block_states->type != MC_NBT_TAG_COMPOUND) continue;
        palette = mc_nbt_compound_get(block_states, "palette");
        if (!palette || palette->type != MC_NBT_TAG_LIST) continue;

        palette_size = palette->payload.list.length;
        if (palette_size <= 0) continue;
        if ((size_t)palette_size > SIZE_MAX / sizeof(*palette_map)) {
            rc = -1;
            goto cleanup;
        }
        palette_map = (mc_global_state_id_t *)mc_arena_alloc(temp_arena, (size_t)palette_size * sizeof(*palette_map));
        if (!palette_map) {
            rc = -1;
            goto cleanup;
        }

        for (int32_t pi = 0; pi < palette_size; pi++) {
            mc_nbt_tag_t *entry = palette->payload.list.items ? palette->payload.list.items[pi] : NULL;
            const mc_nbt_tag_t *name_tag;
            const mc_nbt_tag_t *props_tag;
            char *key;
            mc_global_state_id_t state_id;

            palette_map[pi] = 0;
            if (!entry || entry->type != MC_NBT_TAG_COMPOUND) continue;
            name_tag = mc_nbt_compound_get(entry, "Name");
            props_tag = mc_nbt_compound_get(entry, "Properties");
            if (!name_tag || name_tag->type != MC_NBT_TAG_STRING || !name_tag->payload.string_val) continue;

            key = canonicalize_palette_key_in_arena(name_tag->payload.string_val, props_tag, temp_arena);
            if (!key) {
                rc = -1;
                goto cleanup;
            }

            {
                int32_t runtime_state = mc_world_runtime_state_id_from_key(key, -1);
                if (runtime_state >= 0) {
                    state_id = (mc_global_state_id_t)runtime_state;
                } else {
                    mc_global_state_id_t fallback_state = mc_global_state_id_from_key(key, UINT32_MAX);
                    state_id = (fallback_state != UINT32_MAX) ? fallback_state : 0u;
                }
            }
            palette_map[pi] = state_id;
        }

        data = mc_nbt_compound_get(block_states, "data");
        if (data && data->type == MC_NBT_TAG_LONG_ARRAY && data->payload.long_array.length > 0) {
            longs = data->payload.long_array.data;
            longs_len = data->payload.long_array.length;
        }

        if (palette_size == 1) {
            mc_global_state_id_t global_id = palette_map[0];
            for (int i = 0; i < 4096; i++) {
                int y = i >> 8;
                int rem = i & 255;
                int z = rem >> 4;
                int x = rem & 15;
                int world_y = (sec_idx * 16) + y - 64;
                if (mc_chunk_set_block(out_chunk, x, world_y, z, global_id) != 0) {
                    rc = -1;
                    goto cleanup;
                }
            }
            continue;
        }

        bits_per_block = ceil_log2_u32((uint32_t)palette_size);
        if (bits_per_block < 4) bits_per_block = 4;
        if (!longs || longs_len <= 0) {
            rc = -1;
            goto cleanup;
        }

        for (int i = 0; i < 4096; i++) {
            int y = i >> 8;
            int rem = i & 255;
            int z = rem >> 4;
            int x = rem & 15;
            uint32_t pal_idx = decode_paletted_index(longs, longs_len, bits_per_block, i, NULL);
            int world_y = (sec_idx * 16) + y - 64;

            if ((int32_t)pal_idx < 0 || (int32_t)pal_idx >= palette_size) pal_idx = 0;
            if (mc_chunk_set_block(out_chunk, x, world_y, z, palette_map[pal_idx]) != 0) {
                rc = -1;
                goto cleanup;
            }
        }
    }

    block_entities = mc_nbt_compound_get(root, "block_entities");
    if (block_entities && block_entities->type == MC_NBT_TAG_LIST) {
        for (int32_t i = 0; i < block_entities->payload.list.length; i++) {
            mc_nbt_tag_t *entry = block_entities->payload.list.items ? block_entities->payload.list.items[i] : NULL;
            const mc_nbt_tag_t *id_tag;
            const mc_nbt_tag_t *x_tag;
            const mc_nbt_tag_t *y_tag;
            const mc_nbt_tag_t *z_tag;
            const mc_nbt_tag_t *items_tag;
            mc_block_entity_type_t type;
            mc_pos_t pos;
            mc_block_entity_t entity;

            if (!entry || entry->type != MC_NBT_TAG_COMPOUND) continue;

            id_tag = mc_nbt_compound_get(entry, "id");
            x_tag = mc_nbt_compound_get(entry, "x");
            y_tag = mc_nbt_compound_get(entry, "y");
            z_tag = mc_nbt_compound_get(entry, "z");
            items_tag = mc_nbt_compound_get(entry, "Items");
            if (!id_tag || id_tag->type != MC_NBT_TAG_STRING || !id_tag->payload.string_val) continue;
            if (!x_tag || x_tag->type != MC_NBT_TAG_INT || !y_tag || y_tag->type != MC_NBT_TAG_INT || !z_tag ||
                z_tag->type != MC_NBT_TAG_INT) {
                continue;
            }

            type = block_entity_type_from_id(id_tag->payload.string_val);
            if (type == MC_BLOCK_ENTITY_NONE) continue;

            pos.x = x_tag->payload.int_val;
            pos.y = y_tag->payload.int_val;
            pos.z = z_tag->payload.int_val;
            memset(&entity, 0, sizeof(entity));
            entity.type = type;
            if (type == MC_BLOCK_ENTITY_CHEST || type == MC_BLOCK_ENTITY_BARREL || type == MC_BLOCK_ENTITY_DROPPER ||
                type == MC_BLOCK_ENTITY_SHULKER_BOX || type == MC_BLOCK_ENTITY_ENDER_CHEST ||
                type == MC_BLOCK_ENTITY_FURNACE || type == MC_BLOCK_ENTITY_SMOKER ||
                type == MC_BLOCK_ENTITY_BLAST_FURNACE) {
                bool is_furnace_like = type == MC_BLOCK_ENTITY_FURNACE || type == MC_BLOCK_ENTITY_SMOKER ||
                                       type == MC_BLOCK_ENTITY_BLAST_FURNACE;
                entity.data.container.slot_count = is_furnace_like ? 3u : MC_CONTAINER_SLOT_COUNT;
                if (items_tag && items_tag->type == MC_NBT_TAG_LIST) {
                    for (int32_t si = 0; si < items_tag->payload.list.length; si++) {
                        const mc_nbt_tag_t *slot_entry = items_tag->payload.list.items ? items_tag->payload.list.items[si] : NULL;
                        const mc_nbt_tag_t *slot_tag;
                        const mc_nbt_tag_t *item_id_tag;
                        const mc_nbt_tag_t *count_tag;
                        int32_t slot_index = -1;
                        int32_t item_count = 0;
                        int32_t item_id = -1;
                        if (!slot_entry || slot_entry->type != MC_NBT_TAG_COMPOUND) continue;
                        slot_tag = mc_nbt_compound_get(slot_entry, "slot");
                        if (!slot_tag) slot_tag = mc_nbt_compound_get(slot_entry, "Slot");
                        item_id_tag = mc_nbt_compound_get(slot_entry, "id");
                        count_tag = mc_nbt_compound_get(slot_entry, "count");
                        if (!count_tag) count_tag = mc_nbt_compound_get(slot_entry, "Count");
                        if (nbt_num_to_i32_local(slot_tag, &slot_index) != 0 || slot_index < 0 ||
                            slot_index >= (int32_t)entity.data.container.slot_count) {
                            continue;
                        }
                        if (!item_id_tag || item_id_tag->type != MC_NBT_TAG_STRING) continue;
                        if (nbt_num_to_i32_local(count_tag, &item_count) != 0 || item_count <= 0) continue;
                        item_id = mc_minecraft_item_id(item_id_tag->payload.string_val);
                        if (item_id <= 0) continue;
                        (void)mc_slot_set_simple(&entity.data.container.slots[slot_index], item_id, item_count);
                    }
                }
                if (is_furnace_like) {
                    const mc_nbt_tag_t *tag = mc_nbt_compound_get(entry, "BurnTime");
                    if (!tag) tag = mc_nbt_compound_get(entry, "lit_time_remaining");
                    (void)nbt_num_to_i32_local(tag, &entity.data.container.furnace_burn_time);
                    tag = mc_nbt_compound_get(entry, "BurnDuration");
                    if (!tag) tag = mc_nbt_compound_get(entry, "lit_total_time");
                    (void)nbt_num_to_i32_local(tag, &entity.data.container.furnace_burn_duration);
                    tag = mc_nbt_compound_get(entry, "CookTime");
                    if (!tag) tag = mc_nbt_compound_get(entry, "cooking_time_spent");
                    (void)nbt_num_to_i32_local(tag, &entity.data.container.furnace_cook_time);
                    tag = mc_nbt_compound_get(entry, "CookTimeTotal");
                    if (!tag) tag = mc_nbt_compound_get(entry, "cooking_total_time");
                    (void)nbt_num_to_i32_local(tag, &entity.data.container.furnace_cook_duration);
                }
            }
            if (!mc_be_store_put(be_store, pos, entity)) {
                rc = -1;
                goto cleanup;
            }
        }
    }

    rc = 0;

cleanup:
    mc_arena_reset(temp_arena);
    return rc;
}

int mc_anvil_load_chunk(const char *region_path,
                        int chunk_x,
                        int chunk_z,
                        mc_chunk_t *out_chunk,
                        mc_block_entity_store_t *be_store,
                        mc_arena_t *temp_arena) {
    uint8_t *nbt_buf = NULL;
    size_t nbt_len = 0;
    int local_x;
    int local_z;
    int rc;

    if (!region_path || !out_chunk || !be_store || !temp_arena) return -1;

    local_x = positive_mod_i32(chunk_x, 32);
    local_z = positive_mod_i32(chunk_z, 32);

    rc = mc_anvil_read_chunk_nbt(region_path, local_x, local_z, &nbt_buf, &nbt_len);
    if (rc != 0) return rc;

    rc = mc_anvil_decode_chunk_nbt(nbt_buf, nbt_len, chunk_x, chunk_z, out_chunk, be_store, temp_arena);
    free(nbt_buf);
    return rc;
}

static int deflate_buffer_zlib(const uint8_t *src, size_t src_len, uint8_t **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!src || !out || !out_len) return -1;

    if (src_len > (size_t)UINT_MAX) return -1;
    uLong bound = compressBound((uLong)src_len);
    if (bound == 0) return -1;
    if ((size_t)bound > MC_ANVIL_MAX_CHUNK_NBT) {
        /* bound is pessimistic; still cap to a reasonable size */
        bound = (uLong)MC_ANVIL_MAX_CHUNK_NBT;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)bound);
    if (!buf) return -1;

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.next_in = (Bytef *)src;
    strm.avail_in = (uInt)src_len;
    strm.next_out = buf;
    strm.avail_out = (uInt)bound;

    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        free(buf);
        return -1;
    }

    int ret = deflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END) {
        deflateEnd(&strm);
        free(buf);
        return -1;
    }

    if (deflateEnd(&strm) != Z_OK) {
        free(buf);
        return -1;
    }

    size_t n = (size_t)strm.total_out;
    uint8_t *shrunk = (uint8_t *)realloc(buf, n ? n : 1);
    if (shrunk) buf = shrunk;
    *out = buf;
    *out_len = n;
    return 0;
}

static int write_be32(uint8_t *p, uint32_t v) {
    if (!p) return -1;
    p[0] = (uint8_t)((v >> 24) & 0xFF);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
    return 0;
}

static int write_zeros(FILE *f, size_t n) {
    if (!f) return -1;
    static uint8_t zeros[4096];
    while (n > 0) {
        size_t chunk = n > sizeof(zeros) ? sizeof(zeros) : n;
        if (fwrite(zeros, 1, chunk, f) != chunk) return -1;
        n -= chunk;
    }
    return 0;
}

static uint32_t find_free_sector_run(const uint8_t *header, int skip_idx, uint32_t skip_off, uint8_t skip_cnt, uint8_t required_cnt, uint64_t file_size) {
    if (!header || required_cnt == 0) return 0;

    uint32_t sector_cap = (uint32_t)((file_size + (MC_ANVIL_SECTOR_BYTES - 1)) / MC_ANVIL_SECTOR_BYTES);
    if (sector_cap < 2) sector_cap = 2;

    size_t used_len = (size_t)sector_cap;
    uint8_t *used = (uint8_t *)calloc(used_len, sizeof(*used));
    if (!used) return 0;

    used[0] = 1;
    used[1] = 1;

    for (int i = 0; i < 1024; i++) {
        uint32_t entry = read_be32(header + (size_t)i * 4u);
        uint32_t off = entry >> 8;
        uint8_t cnt = (uint8_t)(entry & 0xFF);
        if (off == 0 || cnt == 0) continue;
        if (i == skip_idx && off == skip_off && cnt == skip_cnt) continue;

        uint64_t end = (uint64_t)off + (uint64_t)cnt;
        if (end > used_len) {
            size_t new_len = (size_t)end;
            uint8_t *next = (uint8_t *)realloc(used, new_len);
            if (!next) {
                free(used);
                return 0;
            }
            memset(next + used_len, 0, new_len - used_len);
            used = next;
            used_len = new_len;
        }
        for (uint32_t s = 0; s < cnt; s++) {
            used[off + s] = 1;
        }
    }

    for (uint32_t start = 2; start + required_cnt <= used_len; start++) {
        bool free_run = true;
        for (uint32_t s = 0; s < required_cnt; s++) {
            if (used[start + s]) {
                free_run = false;
                start += s;
                break;
            }
        }
        if (free_run) {
            free(used);
            return start;
        }
    }

    uint32_t result = (uint32_t)used_len;
    free(used);
    return result;
}

static mc_nbt_tag_t *nbt_new_tag_local(mc_nbt_type_t type, const char *name) {
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

static mc_nbt_tag_t *nbt_new_string_local(const char *name, const char *value) {
    mc_nbt_tag_t *tag = nbt_new_tag_local(MC_NBT_TAG_STRING, name);
    if (!tag) return NULL;
    tag->payload.string_val = strdup(value ? value : "");
    if (!tag->payload.string_val) {
        mc_nbt_free(tag);
        return NULL;
    }
    return tag;
}

static mc_nbt_tag_t *nbt_new_byte_local(const char *name, int8_t value) {
    mc_nbt_tag_t *tag = nbt_new_tag_local(MC_NBT_TAG_BYTE, name);
    if (tag) tag->payload.byte_val = value;
    return tag;
}

static mc_nbt_tag_t *nbt_new_short_local(const char *name, int16_t value) {
    mc_nbt_tag_t *tag = nbt_new_tag_local(MC_NBT_TAG_SHORT, name);
    if (tag) tag->payload.short_val = value;
    return tag;
}

static mc_nbt_tag_t *nbt_new_int_local(const char *name, int32_t value) {
    mc_nbt_tag_t *tag = nbt_new_tag_local(MC_NBT_TAG_INT, name);
    if (tag) tag->payload.int_val = value;
    return tag;
}

static mc_nbt_tag_t *nbt_new_long_array_local(const char *name, const int64_t *src, int32_t len) {
    mc_nbt_tag_t *tag = nbt_new_tag_local(MC_NBT_TAG_LONG_ARRAY, name);
    if (!tag) return NULL;
    tag->payload.long_array.length = len;
    if (len > 0) {
        size_t nbytes = (size_t)len * sizeof(*src);
        tag->payload.long_array.data = (int64_t *)malloc(nbytes);
        if (!tag->payload.long_array.data) {
            mc_nbt_free(tag);
            return NULL;
        }
        memcpy(tag->payload.long_array.data, src, nbytes);
    }
    return tag;
}

static int compound_add_local(mc_nbt_tag_t *compound, mc_nbt_tag_t *child) {
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

static int list_add_local(mc_nbt_tag_t *list, mc_nbt_tag_t *item) {
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

static int key_to_name_and_props(const char *key, char *name_buf, size_t name_cap, mc_nbt_tag_t **out_props) {
    const char *open;
    const char *close;
    size_t name_len;
    mc_nbt_tag_t *props = NULL;

    if (out_props) *out_props = NULL;
    if (!key || !name_buf || name_cap == 0) return -1;

    open = strchr(key, '[');
    if (!open) {
        name_len = strlen(key);
        if (name_len + 1 > name_cap) return -1;
        memcpy(name_buf, key, name_len + 1);
        return 0;
    }

    close = strrchr(open, ']');
    if (!close || close <= open) return -1;
    name_len = (size_t)(open - key);
    if (name_len + 1 > name_cap) return -1;
    memcpy(name_buf, key, name_len);
    name_buf[name_len] = '\0';

    props = nbt_new_tag_local(MC_NBT_TAG_COMPOUND, "Properties");
    if (!props) return -1;

    const char *p = open + 1;
    while (p < close) {
        const char *eq = memchr(p, '=', (size_t)(close - p));
        const char *end = memchr(p, ',', (size_t)(close - p));
        if (!end) end = close;
        if (!eq || eq >= end) {
            mc_nbt_free(props);
            return -1;
        }
        size_t key_len = (size_t)(eq - p);
        size_t val_len = (size_t)(end - eq - 1);
        char prop_name[128];
        char prop_val[128];
        if (key_len == 0 || key_len >= sizeof(prop_name) || val_len >= sizeof(prop_val)) {
            mc_nbt_free(props);
            return -1;
        }
        memcpy(prop_name, p, key_len);
        prop_name[key_len] = '\0';
        memcpy(prop_val, eq + 1, val_len);
        prop_val[val_len] = '\0';
        if (compound_add_local(props, nbt_new_string_local(prop_name, prop_val)) != 0) {
            mc_nbt_free(props);
            return -1;
        }
        p = end + (end < close ? 1 : 0);
    }

    if (out_props) *out_props = props;
    else mc_nbt_free(props);
    return 0;
}

static mc_nbt_tag_t *make_block_state_entry(mc_global_state_id_t state_id) {
    const char *key = mc_global_state_key(state_id);
    char name_buf[256];
    mc_nbt_tag_t *props = NULL;
    mc_nbt_tag_t *entry = NULL;

    if (!key) key = "minecraft:air";
    if (key_to_name_and_props(key, name_buf, sizeof(name_buf), &props) != 0) return NULL;

    entry = nbt_new_tag_local(MC_NBT_TAG_COMPOUND, NULL);
    if (!entry) {
        mc_nbt_free(props);
        return NULL;
    }
    if (compound_add_local(entry, nbt_new_string_local("Name", name_buf)) != 0) {
        mc_nbt_free(props);
        mc_nbt_free(entry);
        return NULL;
    }
    if (props && props->payload.compound.length > 0) {
        if (compound_add_local(entry, props) != 0) {
            mc_nbt_free(props);
            mc_nbt_free(entry);
            return NULL;
        }
    } else {
        mc_nbt_free(props);
    }
    return entry;
}

static int build_section_block_states(const mc_chunk_t *chunk, int sec_idx, mc_nbt_tag_t **out) {
    mc_nbt_tag_t *block_states = NULL;
    mc_nbt_tag_t *palette = NULL;
    uint32_t values[4096];
    mc_global_state_id_t palette_ids[4096];
    int32_t palette_len = 0;
    int bits;
    int64_t *packed = NULL;
    int32_t packed_len = 0;

    if (out) *out = NULL;
    if (!chunk || sec_idx < 0 || sec_idx >= MC_WORLD_SECTION_COUNT || !out) return -1;

    for (int i = 0; i < 4096; i++) {
        int y = i >> 8;
        int rem = i & 255;
        int z = rem >> 4;
        int x = rem & 15;
        int world_y = (sec_idx * 16) + y + MC_WORLD_MIN_Y;
        mc_global_state_id_t state_id = mc_chunk_get_block(chunk, x, world_y, z);
        int found = -1;
        for (int32_t pi = 0; pi < palette_len; pi++) {
            if (palette_ids[pi] == state_id) {
                found = pi;
                break;
            }
        }
        if (found < 0) {
            found = palette_len;
            palette_ids[palette_len++] = state_id;
        }
        values[i] = (uint32_t)found;
    }

    block_states = nbt_new_tag_local(MC_NBT_TAG_COMPOUND, "block_states");
    palette = nbt_new_tag_local(MC_NBT_TAG_LIST, "palette");
    if (!block_states || !palette) goto fail;
    palette->payload.list.elem_type = MC_NBT_TAG_COMPOUND;

    for (int32_t i = 0; i < palette_len; i++) {
        mc_nbt_tag_t *entry = make_block_state_entry(palette_ids[i]);
        if (!entry || list_add_local(palette, entry) != 0) {
            mc_nbt_free(entry);
            goto fail;
        }
    }
    if (compound_add_local(block_states, palette) != 0) goto fail;
    palette = NULL;

    if (palette_len > 1) {
        bits = ceil_log2_u32((uint32_t)palette_len);
        if (bits < 4) bits = 4;
        if (mc_packed_pack_compact_u32(values, 4096, bits, &packed, &packed_len) != 0) goto fail;
        mc_nbt_tag_t *data = nbt_new_long_array_local("data", packed, packed_len);
        if (!data || compound_add_local(block_states, data) != 0) {
            mc_nbt_free(data);
            goto fail;
        }
    }

    *out = block_states;
    free(packed);
    return 0;

fail:
    free(packed);
    mc_nbt_free(palette);
    mc_nbt_free(block_states);
    return -1;
}

static int build_section_biomes(mc_nbt_tag_t **out) {
    mc_nbt_tag_t *biomes = NULL;
    mc_nbt_tag_t *palette = NULL;

    if (out) *out = NULL;
    if (!out) return -1;

    biomes = nbt_new_tag_local(MC_NBT_TAG_COMPOUND, "biomes");
    palette = nbt_new_tag_local(MC_NBT_TAG_LIST, "palette");
    if (!biomes || !palette) goto fail;
    palette->payload.list.elem_type = MC_NBT_TAG_STRING;
    if (list_add_local(palette, nbt_new_string_local(NULL, "minecraft:plains")) != 0) goto fail;
    if (compound_add_local(biomes, palette) != 0) goto fail;
    palette = NULL;
    *out = biomes;
    return 0;

fail:
    mc_nbt_free(palette);
    mc_nbt_free(biomes);
    return -1;
}

static const char *block_entity_id_for_state(mc_global_state_id_t state_id) {
    const char *key = mc_global_state_key(state_id);
    const char *props = NULL;
    static char name_buf[128];
    size_t base_len = 0;
    if (!key) return NULL;
    props = strchr(key, '[');
    base_len = props ? (size_t)(props - key) : strlen(key);
    if (base_len > 0 && base_len < sizeof(name_buf)) {
        memcpy(name_buf, key, base_len);
        name_buf[base_len] = '\0';
        if (mc_minecraft_block_entity_type_id(name_buf) >= 0) return name_buf;
    }
    if (strstr(key, "ender_chest")) return "minecraft:ender_chest";
    if (strstr(key, "trapped_chest")) return "minecraft:trapped_chest";
    if (strstr(key, "chest")) return "minecraft:chest";
    if (strstr(key, "barrel")) return "minecraft:barrel";
    if (strstr(key, "blast_furnace")) return "minecraft:blast_furnace";
    if (strstr(key, "smoker")) return "minecraft:smoker";
    if (strstr(key, "furnace")) return "minecraft:furnace";
    if (strstr(key, "dropper")) return "minecraft:dropper";
    if (strstr(key, "dispenser")) return "minecraft:dispenser";
    if (strstr(key, "shulker_box")) return "minecraft:shulker_box";
    if (strstr(key, "sign")) return "minecraft:sign";
    return NULL;
}

static int build_block_entities_list(const mc_chunk_t *chunk, const mc_block_entity_store_t *be_store, mc_nbt_tag_t **out) {
    mc_nbt_tag_t *list = NULL;
    int32_t base_x;
    int32_t base_z;

    if (out) *out = NULL;
    if (!chunk || !out) return -1;

    list = nbt_new_tag_local(MC_NBT_TAG_LIST, "block_entities");
    if (!list) return -1;
    list->payload.list.elem_type = MC_NBT_TAG_COMPOUND;
    base_x = chunk->cx * MC_CHUNK_XZ;
    base_z = chunk->cz * MC_CHUNK_XZ;

    if (be_store && be_store->states) {
        for (size_t i = 0; i < be_store->cap; i++) {
            if (be_store->states[i] != 1) continue;
            mc_pos_t pos = be_store->positions[i];
            if (pos.x < base_x || pos.x >= base_x + MC_CHUNK_XZ || pos.z < base_z || pos.z >= base_z + MC_CHUNK_XZ) continue;
            int lx = pos.x - base_x;
            int lz = pos.z - base_z;
            mc_global_state_id_t sid = mc_chunk_get_block(chunk, lx, pos.y, lz);
            const char *id_name = block_entity_id_for_state(sid);
            mc_nbt_tag_t *entry = NULL;
            if (!id_name) continue;
            entry = nbt_new_tag_local(MC_NBT_TAG_COMPOUND, NULL);
            if (!entry) goto fail;
            if (compound_add_local(entry, nbt_new_string_local("id", id_name)) != 0 ||
                compound_add_local(entry, nbt_new_int_local("x", pos.x)) != 0 ||
                compound_add_local(entry, nbt_new_int_local("y", pos.y)) != 0 ||
                compound_add_local(entry, nbt_new_int_local("z", pos.z)) != 0) {
                mc_nbt_free(entry);
                goto fail;
            }
            if (be_store->entities[i].type == MC_BLOCK_ENTITY_CHEST || be_store->entities[i].type == MC_BLOCK_ENTITY_BARREL ||
                be_store->entities[i].type == MC_BLOCK_ENTITY_DROPPER || be_store->entities[i].type == MC_BLOCK_ENTITY_SHULKER_BOX ||
                be_store->entities[i].type == MC_BLOCK_ENTITY_ENDER_CHEST || be_store->entities[i].type == MC_BLOCK_ENTITY_FURNACE ||
                be_store->entities[i].type == MC_BLOCK_ENTITY_SMOKER || be_store->entities[i].type == MC_BLOCK_ENTITY_BLAST_FURNACE) {
                bool is_furnace_like = be_store->entities[i].type == MC_BLOCK_ENTITY_FURNACE ||
                                       be_store->entities[i].type == MC_BLOCK_ENTITY_SMOKER ||
                                       be_store->entities[i].type == MC_BLOCK_ENTITY_BLAST_FURNACE;
                mc_nbt_tag_t *items = nbt_new_tag_local(MC_NBT_TAG_LIST, "Items");
                if (!items) {
                    mc_nbt_free(entry);
                    goto fail;
                }
                items->payload.list.elem_type = MC_NBT_TAG_COMPOUND;
                uint32_t slot_count = be_store->entities[i].data.container.slot_count;
                if (slot_count > MC_CONTAINER_SLOT_COUNT) slot_count = MC_CONTAINER_SLOT_COUNT;
                if (is_furnace_like && slot_count > 3u) slot_count = 3u;
                for (uint32_t slot = 0; slot < slot_count; slot++) {
                    const mc_slot_t *src = &be_store->entities[i].data.container.slots[slot];
                    const char *item_name;
                    mc_nbt_tag_t *slot_entry;
                    if (!src->present || src->count <= 0 || src->item_id <= 0) continue;
                    item_name = mc_minecraft_item_name(src->item_id);
                    if (!item_name || !*item_name) continue;
                    slot_entry = nbt_new_tag_local(MC_NBT_TAG_COMPOUND, NULL);
                    if (!slot_entry ||
                        compound_add_local(slot_entry, nbt_new_byte_local("slot", (int8_t)slot)) != 0 ||
                        compound_add_local(slot_entry, nbt_new_string_local("id", item_name)) != 0 ||
                        compound_add_local(slot_entry, nbt_new_byte_local("count", (int8_t)src->count)) != 0 ||
                        list_add_local(items, slot_entry) != 0) {
                        mc_nbt_free(slot_entry);
                        mc_nbt_free(items);
                        mc_nbt_free(entry);
                        goto fail;
                    }
                }
                if (compound_add_local(entry, items) != 0) {
                    mc_nbt_free(items);
                    mc_nbt_free(entry);
                    goto fail;
                }
                if (is_furnace_like &&
                    (compound_add_local(entry, nbt_new_short_local("BurnTime",
                                                                   (int16_t)be_store->entities[i].data.container.furnace_burn_time)) != 0 ||
                     compound_add_local(entry, nbt_new_short_local("BurnDuration",
                                                                   (int16_t)be_store->entities[i].data.container.furnace_burn_duration)) != 0 ||
                     compound_add_local(entry, nbt_new_short_local("CookTime",
                                                                   (int16_t)be_store->entities[i].data.container.furnace_cook_time)) != 0 ||
                     compound_add_local(entry, nbt_new_short_local("CookTimeTotal",
                                                                   (int16_t)be_store->entities[i].data.container.furnace_cook_duration)) != 0)) {
                    mc_nbt_free(entry);
                    goto fail;
                }
            }
            if (list_add_local(list, entry) != 0) {
                mc_nbt_free(entry);
                goto fail;
            }
        }
    }

    *out = list;
    return 0;

fail:
    mc_nbt_free(list);
    return -1;
}

static int build_chunk_nbt_root(const mc_chunk_t *chunk, const mc_block_entity_store_t *be_store, mc_nbt_tag_t **out_root) {
    mc_nbt_tag_t *root = NULL;
    mc_nbt_tag_t *sections = NULL;
    mc_nbt_tag_t *block_entities = NULL;
    const int32_t min_section_y = MC_WORLD_MIN_Y / 16;

    if (out_root) *out_root = NULL;
    if (!chunk || !out_root) return -1;

    root = nbt_new_tag_local(MC_NBT_TAG_COMPOUND, "");
    sections = nbt_new_tag_local(MC_NBT_TAG_LIST, "sections");
    block_entities = NULL;
    if (!root || !sections) goto fail;
    sections->payload.list.elem_type = MC_NBT_TAG_COMPOUND;

    for (int sec_idx = 0; sec_idx < MC_WORLD_SECTION_COUNT; sec_idx++) {
        mc_nbt_tag_t *section = nbt_new_tag_local(MC_NBT_TAG_COMPOUND, NULL);
        mc_nbt_tag_t *block_states = NULL;
        mc_nbt_tag_t *biomes = NULL;
        if (!section) goto fail;
        if (compound_add_local(section, nbt_new_byte_local("Y", (int8_t)(min_section_y + sec_idx))) != 0) {
            mc_nbt_free(section);
            goto fail;
        }
        if (build_section_block_states(chunk, sec_idx, &block_states) != 0 ||
            build_section_biomes(&biomes) != 0 ||
            compound_add_local(section, block_states) != 0 ||
            compound_add_local(section, biomes) != 0 ||
            list_add_local(sections, section) != 0) {
            mc_nbt_free(block_states);
            mc_nbt_free(biomes);
            mc_nbt_free(section);
            goto fail;
        }
    }

    if (build_block_entities_list(chunk, be_store, &block_entities) != 0) goto fail;

    if (compound_add_local(root, nbt_new_int_local("xPos", chunk->cx)) != 0 ||
        compound_add_local(root, nbt_new_int_local("yPos", min_section_y)) != 0 ||
        compound_add_local(root, nbt_new_int_local("zPos", chunk->cz)) != 0 ||
        compound_add_local(root, nbt_new_int_local("DataVersion", MC_ANVIL_EXPECTED_DATA_VERSION)) != 0 ||
        compound_add_local(root, nbt_new_string_local("Status", "minecraft:full")) != 0 ||
        compound_add_local(root, sections) != 0 ||
        compound_add_local(root, block_entities) != 0) {
        goto fail;
    }

    *out_root = root;
    return 0;

fail:
    mc_nbt_free(block_entities);
    mc_nbt_free(sections);
    mc_nbt_free(root);
    return -1;
}

int mc_anvil_write_chunk(const char *region_path, const mc_chunk_t *chunk, const mc_block_entity_store_t *be_store) {
    mc_nbt_tag_t *root = NULL;
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    int local_x;
    int local_z;
    int rc = -1;

    if (!region_path || !chunk) return -1;
    local_x = positive_mod_i32(chunk->cx, 32);
    local_z = positive_mod_i32(chunk->cz, 32);

    if (build_chunk_nbt_root(chunk, be_store, &root) != 0) goto cleanup;
    if (mc_nbt_write_named_root(root, &raw, &raw_len) != 0) goto cleanup;
    rc = mc_anvil_write_chunk_nbt(region_path, local_x, local_z, raw, raw_len);

cleanup:
    free(raw);
    mc_nbt_free(root);
    return rc;
}

int mc_anvil_encode_chunk_nbt(const mc_chunk_t *chunk, const mc_block_entity_store_t *be_store, uint8_t **out_raw, size_t *out_raw_len) {
    mc_nbt_tag_t *root = NULL;
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    int rc = -1;

    if (out_raw) *out_raw = NULL;
    if (out_raw_len) *out_raw_len = 0;
    if (!chunk || !out_raw || !out_raw_len) return -1;

    if (build_chunk_nbt_root(chunk, be_store, &root) != 0) goto cleanup;
    if (mc_nbt_write_named_root(root, &raw, &raw_len) != 0) goto cleanup;

    *out_raw = raw;
    *out_raw_len = raw_len;
    raw = NULL;
    rc = 0;

cleanup:
    free(raw);
    mc_nbt_free(root);
    return rc;
}

int mc_anvil_write_chunk_nbt(const char *region_path, int local_x, int local_z, const uint8_t *nbt, size_t nbt_len) {
    if (!region_path || !nbt) return -1;
    if (local_x < 0 || local_x > 31 || local_z < 0 || local_z > 31) return -1;

    FILE *f = fopen(region_path, "r+b");
    bool new_file = false;
    if (!f) {
        f = fopen(region_path, "w+b");
        new_file = true;
    }
    if (!f) return -1;

    if (new_file) {
        uint8_t header[MC_ANVIL_HEADER_BYTES];
        memset(header, 0, sizeof(header));
        if (fwrite(header, 1, sizeof(header), f) != sizeof(header)) {
            fclose(f);
            return -1;
        }
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long file_size_l = ftell(f);
    if (file_size_l < 0) {
        fclose(f);
        return -1;
    }
    uint64_t file_size = (uint64_t)file_size_l;
    if (file_size < MC_ANVIL_HEADER_BYTES) {
        fclose(f);
        return -1;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    uint8_t header[MC_ANVIL_HEADER_BYTES];
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return -1;
    }

    uint8_t *comp = NULL;
    size_t comp_len = 0;
    if (deflate_buffer_zlib(nbt, nbt_len, &comp, &comp_len) != 0) {
        fclose(f);
        return -1;
    }

    if (comp_len + 5 > MC_ANVIL_MAX_CHUNK_NBT) {
        free(comp);
        fclose(f);
        return -1;
    }

    size_t total = comp_len + 5; /* len(4) + type(1) + payload */
    size_t required = (total + MC_ANVIL_SECTOR_BYTES - 1) / MC_ANVIL_SECTOR_BYTES;
    if (required == 0) required = 1;
    if (required > 255) {
        free(comp);
        fclose(f);
        return -1;
    }
    uint8_t required_sectors = (uint8_t)required;

    int idx = local_x + local_z * 32;
    const uint8_t *ent = header + (idx * 4);
    uint32_t entry = read_be32(ent);
    uint32_t old_off = entry >> 8;
    uint8_t old_cnt = (uint8_t)(entry & 0xFF);

    uint32_t new_off = 0;
    uint8_t new_cnt = required_sectors;

    if (old_off != 0 && old_cnt != 0 && old_cnt >= required_sectors) {
        new_off = old_off;
    } else {
        new_off = find_free_sector_run(header, idx, old_off, old_cnt, required_sectors, file_size);
        if (new_off < 2) new_off = 2;
        if (new_off > 0xFFFFFFu) {
            free(comp);
            fclose(f);
            return -1;
        }
    }

    uint64_t chunk_off_bytes = (uint64_t)new_off * MC_ANVIL_SECTOR_BYTES;
    if (fseek(f, (long)chunk_off_bytes, SEEK_SET) != 0) {
        free(comp);
        fclose(f);
        return -1;
    }

    uint8_t chunk_hdr[5];
    write_be32(chunk_hdr, (uint32_t)(comp_len + 1));
    chunk_hdr[4] = 0x02; /* zlib */
    if (fwrite(chunk_hdr, 1, sizeof(chunk_hdr), f) != sizeof(chunk_hdr)) {
        free(comp);
        fclose(f);
        return -1;
    }
    if (comp_len > 0 && fwrite(comp, 1, comp_len, f) != comp_len) {
        free(comp);
        fclose(f);
        return -1;
    }
    free(comp);

    size_t pad = (size_t)new_cnt * MC_ANVIL_SECTOR_BYTES - total;
    if (pad > 0) {
        if (write_zeros(f, pad) != 0) {
            fclose(f);
            return -1;
        }
    }

    /* Update header offsets entry (3 bytes offset + 1 byte sector count). */
    header[idx * 4 + 0] = (uint8_t)((new_off >> 16) & 0xFF);
    header[idx * 4 + 1] = (uint8_t)((new_off >> 8) & 0xFF);
    header[idx * 4 + 2] = (uint8_t)(new_off & 0xFF);
    header[idx * 4 + 3] = new_cnt;

    /* Update timestamp (seconds since epoch, BE). */
    uint32_t ts = (uint32_t)time(NULL);
    uint8_t *tsp = header + MC_ANVIL_OFFSETS_BYTES + (idx * 4);
    write_be32(tsp, ts);

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    if (fwrite(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return -1;
    }
    fflush(f);
    fclose(f);
    return 0;
}
