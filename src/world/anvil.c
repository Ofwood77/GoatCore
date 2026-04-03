#include "mc_anvil.h"

#include "block_registry.h"
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
        return MC_BLOCK_ENTITY_CHEST;
    }
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

int mc_anvil_load_chunk(const char *region_path,
                        int chunk_x,
                        int chunk_z,
                        mc_chunk_t *out_chunk,
                        mc_block_entity_store_t *be_store,
                        mc_arena_t *temp_arena) {
    uint8_t *nbt_buf = NULL;
    size_t nbt_len = 0;
    size_t bytes_read = 0;
    mc_nbt_tag_t *root = NULL;
    const mc_nbt_tag_t *sections = NULL;
    const mc_nbt_tag_t *block_entities = NULL;
    int rc = -1;
    int local_x;
    int local_z;

    if (!region_path || !out_chunk || !be_store || !temp_arena) return -1;

    local_x = positive_mod_i32(chunk_x, 32);
    local_z = positive_mod_i32(chunk_z, 32);

    rc = mc_anvil_read_chunk_nbt(region_path, local_x, local_z, &nbt_buf, &nbt_len);
    if (rc != 0) goto cleanup;

    if (mc_nbt_read_named_root_arena(nbt_buf, nbt_len, temp_arena, &root, &bytes_read) != 0 || !root) {
        rc = -1;
        goto cleanup;
    }
    if (!mc_anvil_validate_chunk(root)) {
        rc = -1;
        goto cleanup;
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
            mc_block_entity_type_t type;
            mc_pos_t pos;
            mc_block_entity_t entity;

            if (!entry || entry->type != MC_NBT_TAG_COMPOUND) continue;

            id_tag = mc_nbt_compound_get(entry, "id");
            x_tag = mc_nbt_compound_get(entry, "x");
            y_tag = mc_nbt_compound_get(entry, "y");
            z_tag = mc_nbt_compound_get(entry, "z");
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
            if (!mc_be_store_put(be_store, pos, entity)) {
                rc = -1;
                goto cleanup;
            }
        }
    }

    rc = 0;

cleanup:
    free(nbt_buf);
    mc_arena_reset(temp_arena);
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
    uint8_t new_cnt = 0;

    if (old_off != 0 && old_cnt != 0 && old_cnt >= required_sectors) {
        new_off = old_off;
        new_cnt = old_cnt;
    } else {
        uint64_t aligned = (file_size + (MC_ANVIL_SECTOR_BYTES - 1)) / MC_ANVIL_SECTOR_BYTES;
        if (aligned < 2) aligned = 2;
        if (aligned > 0xFFFFFFu) {
            free(comp);
            fclose(f);
            return -1;
        }
        new_off = (uint32_t)aligned;
        new_cnt = required_sectors;
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
