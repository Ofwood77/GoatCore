/* S3C offline translation; never linked into the simulation or storage path. */
#include "mc_chunk_migration.h"
#include "mc_anvil.h"
#include "mc_packed.h"
#include "block_registry.h"
#include "generated_native_be_blocks.h"
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mc_nbt_tag_t *field(mc_nbt_tag_t *root, const char *name) {
    return (mc_nbt_tag_t *)mc_nbt_compound_get(root, name);
}
static bool unique(const mc_nbt_tag_t *t, unsigned depth) {
    if (!t || depth > 64)
        return false;
    if (t->type == MC_NBT_TAG_COMPOUND) {
        if (t->payload.compound.length > 4096)
            return false;
        for (int32_t i = 0; i < t->payload.compound.length; i++) {
            const mc_nbt_tag_t *a = t->payload.compound.children[i];
            if (!a || !a->name || !unique(a, depth + 1))
                return false;
            for (int32_t j = 0; j < i; j++)
                if (!strcmp(a->name, t->payload.compound.children[j]->name))
                    return false;
        }
    } else if (t->type == MC_NBT_TAG_LIST)
        for (int32_t i = 0; i < t->payload.list.length; i++)
            if (!unique(t->payload.list.items[i], depth + 1))
                return false;
    return true;
}
static int bits_for(unsigned n) {
    int bits = 0;
    for (unsigned v = n - 1; v; v >>= 1)
        bits++;
    return bits < 4 ? 4 : bits;
}
static bool requires_entity(uint32_t state) {
    if (state >= GLOBAL_BLOCK_STATES_COUNT)
        return true;
    unsigned b = GLOBAL_BLOCK_STATES[state].block_index;
    if (b >= sizeof(native_sensitive_by_block) || !native_sensitive_by_block[b])
        return false;
    const char *name = GLOBAL_BLOCKS[b].name;
    for (size_t i = 0; i < sizeof(native_be_blocks) / sizeof(*native_be_blocks); i++)
        if (!strcmp(name, native_be_blocks[i].block))
            return true;
    return false;
}
int mc_chunk_migrate_nbt(const uint8_t *raw, size_t length, int32_t cx, int32_t cz, uint8_t **output,
                         size_t *output_length, mc_chunk_migration_info_t *info, char reason[160]) {
    mc_nbt_tag_t *root = NULL;
    uint8_t *copy = NULL, *result = NULL;
    size_t used = 0, n = 0;
    int rc = -1;
    mc_chunk_t native = {0}, legacy = {0};
    bool have_native = false, have_legacy = false, have_arena = false;
    mc_arena_t arena;
    mc_block_entity_store_t entities;
    mc_be_store_init(&entities);
    if (!output || !output_length || !info || !reason) {
        errno = EINVAL;
        return -1;
    }
    *output = NULL;
    *output_length = 0;
    memset(info, 0, sizeof(*info));
#define REJECT(why)                                                                                          \
    do {                                                                                                     \
        snprintf(reason, 160, "%s", why);                                                                    \
        errno = EOPNOTSUPP;                                                                                  \
        goto done;                                                                                           \
    } while (0)
    snprintf(reason, 160, "allocation or codec failure");
    if (!raw || !length || length > MC_MIGRATION_MAX_NBT)
        REJECT("NBT exceeds supported bound or is empty");
    if (cx < INT32_MIN / 16 || cx > (INT32_MAX - 15) / 16 || cz < INT32_MIN / 16 ||
        cz > (INT32_MAX - 15) / 16)
        REJECT("chunk coordinate exceeds safe block-coordinate range");
    if (mc_nbt_read_named_root(raw, length, &root, &used) || used != length ||
        root->type != MC_NBT_TAG_COMPOUND || !unique(root, 0))
        REJECT("invalid, trailing, duplicate or excessively nested NBT");
    if (mc_nbt_write_named_root(root, &copy, &n))
        goto done;
    if (n != length || memcmp(raw, copy, n))
        REJECT("NBT cannot roundtrip exactly");
    free(copy);
    copy = NULL;
    mc_nbt_tag_t *version = field(root, "DataVersion");
    if (version && version->type == MC_NBT_TAG_INT)
        info->source_version = version->payload.int_val;
    if (!version || version->type != MC_NBT_TAG_INT ||
        (version->payload.int_val != 4785 && version->payload.int_val != 4788))
        REJECT("unsupported DataVersion");
    info->source_version = version->payload.int_val;
    mc_nbt_tag_t *x = field(root, "xPos"), *z = field(root, "zPos"), *status = field(root, "Status");
    if (!x || x->type != MC_NBT_TAG_INT || x->payload.int_val != cx || !z || z->type != MC_NBT_TAG_INT ||
        z->payload.int_val != cz)
        REJECT("source xPos/zPos mismatch filename");
    if (!status || status->type != MC_NBT_TAG_STRING || strcmp(status->payload.string_val, "minecraft:full"))
        REJECT("unsupported generation status; protochunks cannot be promoted");
    mc_nbt_tag_t *secs = field(root, "sections");
    if (!secs || secs->type != MC_NBT_TAG_LIST || secs->payload.list.elem_type != MC_NBT_TAG_COMPOUND)
        REJECT("invalid sections");
    for (int32_t i = 0; i < secs->payload.list.length; i++) {
        mc_nbt_tag_t *sec = secs->payload.list.items[i], *bs = field(sec, "block_states"),
                     *bio = field(sec, "biomes");
        if (bio) {
            mc_nbt_tag_t *p = field(bio, "palette");
            if (bio->type != MC_NBT_TAG_COMPOUND || !p || p->type != MC_NBT_TAG_LIST ||
                p->payload.list.elem_type != MC_NBT_TAG_STRING || p->payload.list.length != 1 ||
                strcmp(p->payload.list.items[0]->payload.string_val, "minecraft:plains"))
                REJECT("legacy biome provenance unsupported (only uniform plains)");
        }
        const char *lights[] = {"BlockLight", "SkyLight"};
        for (unsigned k = 0; k < 2; k++) {
            mc_nbt_tag_t *t = field(sec, lights[k]);
            if (t && (t->type != MC_NBT_TAG_BYTE_ARRAY || t->payload.byte_array.length != 2048))
                REJECT("invalid light array");
        }
        if (!bs)
            continue;
        mc_nbt_tag_t *p = field(bs, "palette"), *data = field(bs, "data");
        if (bs->type != MC_NBT_TAG_COMPOUND || !p || p->type != MC_NBT_TAG_LIST ||
            p->payload.list.elem_type != MC_NBT_TAG_COMPOUND || p->payload.list.length < 1 ||
            p->payload.list.length > 4096)
            REJECT("invalid block-state palette");
        unsigned count = (unsigned)p->payload.list.length;
        if (count == 1) {
            if (data && (data->type != MC_NBT_TAG_LONG_ARRAY || data->payload.long_array.length))
                REJECT("unexpected singleton data");
            continue;
        }
        int bits = bits_for(count);
        unsigned per = 64u / (unsigned)bits;
        size_t compact = mc_packed_compact_long_count(4096, bits), padded = (4096 + per - 1) / per;
        if (!data || data->type != MC_NBT_TAG_LONG_ARRAY ||
            (data->payload.long_array.length != (int32_t)compact &&
             data->payload.long_array.length != (int32_t)padded))
            REJECT("invalid compact/padded block-state length");
        bool old = (size_t)data->payload.long_array.length == compact;
        if (old)
            info->compact_sections++;
        else
            info->padded_sections++;
        int64_t *next = calloc(padded, sizeof(*next));
        if (!next)
            goto done;
        uint64_t mask = (UINT64_C(1) << bits) - 1;
        for (unsigned cell = 0; cell < 4096; cell++) {
            uint32_t value = old ? mc_packed_unpack_compact_u32(data->payload.long_array.data,
                                                                data->payload.long_array.length, cell, bits)
                                 : (uint32_t)(((uint64_t)data->payload.long_array.data[cell / per] >>
                                               ((cell % per) * bits)) &
                                              mask);
            if (value >= count) {
                free(next);
                REJECT("palette index out of bounds (legacy fallback forbidden)");
            }
            next[cell / per] =
                (int64_t)((uint64_t)next[cell / per] | ((uint64_t)value << ((cell % per) * bits)));
        }
        /* Refuse hidden nonzero padding instead of silently discarding it. */
        if (old && compact * 64 > 4096u * (unsigned)bits &&
            ((uint64_t)data->payload.long_array.data[compact - 1] >> (4096u * (unsigned)bits % 64))) {
            free(next);
            REJECT("nonzero compact padding");
        }
        if (!old && memcmp(data->payload.long_array.data, next, padded * sizeof(*next))) {
            free(next);
            REJECT("nonzero padded data bits");
        }
        free(data->payload.long_array.data);
        data->payload.long_array.data = next;
        data->payload.long_array.length = (int32_t)padded;
    }
    mc_nbt_tag_t *hm = field(root, "Heightmaps");
    mc_nbt_tag_t *light = field(root, "isLightOn");
    if (light &&
        (light->type != MC_NBT_TAG_BYTE || (light->payload.byte_val != 0 && light->payload.byte_val != 1)))
        REJECT("invalid isLightOn flag");
    if (hm) {
        if (hm->type != MC_NBT_TAG_COMPOUND)
            REJECT("invalid Heightmaps");
        const char *known[] = {"WORLD_SURFACE_WG", "WORLD_SURFACE",   "OCEAN_FLOOR_WG",
                               "OCEAN_FLOOR",      "MOTION_BLOCKING", "MOTION_BLOCKING_NO_LEAVES"};
        for (unsigned k = 0; k < 6; k++) {
            mc_nbt_tag_t *t = field(hm, known[k]);
            if (!t)
                continue;
            if (t->type != MC_NBT_TAG_LONG_ARRAY || t->payload.long_array.length != 37)
                REJECT("heightmap packing unsupported; no invented rebuild");
            for (unsigned cell = 0; cell < 256; cell++)
                if ((((uint64_t)t->payload.long_array.data[cell / 7] >> ((cell % 7) * 9)) & 511) > 384)
                    REJECT("heightmap value out of range");
        }
    }
    version->payload.int_val = 4788;
    if (mc_nbt_write_named_root(root, &result, &n))
        goto done;
    if (mc_anvil_decode_native(result, n, cx, cz, &native))
        REJECT("native schema/state/BE validation refused");
    have_native = true;
    if (native.storage_readonly)
        REJECT("scheduled ticks, active or unsupported block entities");
    mc_nbt_tag_t *bes = field(root, "block_entities");
    uint8_t seen[MC_BLOCKS_PER_CHUNK / 8] = {0};
    if (bes) {
        info->block_entities = (unsigned)bes->payload.list.length;
        for (int32_t i = 0; i < bes->payload.list.length; i++) {
            mc_nbt_tag_t *be = bes->payload.list.items[i];
            const char *old_timers[] = {"BurnTime", "BurnDuration", "CookTime", "CookTimeTotal"};
            for (unsigned k = 0; k < 4; k++) {
                mc_nbt_tag_t *t = field(be, old_timers[k]);
                if (t && (t->type != MC_NBT_TAG_SHORT || t->payload.short_val))
                    REJECT("active/ambiguous legacy furnace timers");
            }
            int x = field(be, "x")->payload.int_val, y = field(be, "y")->payload.int_val,
                z = field(be, "z")->payload.int_val;
            unsigned cell = (unsigned)(y + 64) * 256 + (unsigned)(z - cz * 16) * 16 + (unsigned)(x - cx * 16);
            seen[cell / 8] |= (uint8_t)(1u << (cell % 8));
        }
    }
    /* Native validation checks listed entities; migration also refuses blocks
     * whose absent BE would otherwise be invented by Vanilla on first load. */
    for (unsigned sy = 0; sy < MC_WORLD_SECTION_COUNT; sy++) {
        bool need = false;
        mc_paletted_container_t *sec = &native.sections[sy];
        if (sec->is_direct)
            need = true;
        else if (!sec->palette_len)
            need = requires_entity(sec->uniform_state);
        for (size_t p = 0; p < sec->palette_len; p++)
            need |= requires_entity(sec->palette[p]);
        if (!need)
            continue;
        for (unsigned cell = 0; cell < 4096; cell++) {
            unsigned at = sy * 4096 + cell;
            uint32_t id =
                mc_chunk_get_block(&native, cell & 15, (int)(sy * 16 + (cell >> 8)) - 64, (cell >> 4) & 15);
            if (requires_entity(id) && !(seen[at / 8] & (1u << (at % 8))))
                REJECT("block entity missing; Vanilla would synthesize data");
        }
    }
    if (mc_chunk_init(&legacy, cx, cz, 0))
        goto done;
    have_legacy = true;
    if (mc_arena_init(&arena, 2u * 1024u * 1024u))
        goto done;
    have_arena = true;
    if (mc_anvil_decode_chunk_nbt(raw, length, cx, cz, &legacy, &entities, &arena))
        REJECT("historical codec refused source");
    for (int y = -64; y < 320; y++)
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++)
                if (mc_chunk_get_block(&native, x, y, z) != mc_chunk_get_block(&legacy, x, y, z))
                    REJECT("historical/native block-state mismatch");
    *output = result;
    result = NULL;
    *output_length = n;
    snprintf(reason, 160, "supported; all 98304 block states match");
    rc = 0;
done:;
    int saved = errno;
    free(copy);
    free(result);
    mc_nbt_free(root);
    if (have_arena)
        mc_arena_destroy(&arena);
    if (have_legacy)
        mc_chunk_destroy(&legacy);
    if (have_native)
        mc_chunk_destroy(&native);
    mc_be_store_destroy(&entities);
    errno = saved;
    return rc;
#undef REJECT
}
