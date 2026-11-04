#include "g1_internal.h"
#include "mc_nbt.h"
#include "mc_packed.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
/* Lossless optional preprocessing: only canonical block-state long arrays.
 * Palette entries and every other NBT field remain untouched. Unsupported
 * structure returns 1 and caller stores the exact original byte stream. */
static mc_nbt_tag_t *field(mc_nbt_tag_t *t, const char *name) {
    return (mc_nbt_tag_t *)mc_nbt_compound_get(t, name);
}
int g1_pack(const uint8_t *raw, size_t length, bool inverse, uint8_t **out, size_t *n) {
    mc_nbt_tag_t *root = NULL;
    size_t used = 0;
    int rc = 1;
    *out = NULL;
    *n = 0;
    if (mc_nbt_read_named_root(raw, length, &root, &used) || used != length) {
        rc = -1;
        goto done;
    }
    mc_nbt_tag_t *sections = field(root, "sections");
    if (!sections || sections->type != MC_NBT_TAG_LIST ||
        sections->payload.list.elem_type != MC_NBT_TAG_COMPOUND || sections->payload.list.length > 512)
        goto done;
    for (int32_t i = 0; i < sections->payload.list.length; i++) {
        mc_nbt_tag_t *bs = field(sections->payload.list.items[i], "block_states");
        if (!bs)
            continue;
        mc_nbt_tag_t *p = field(bs, "palette"), *data = field(bs, "data");
        if (!p || p->type != MC_NBT_TAG_LIST || p->payload.list.elem_type != MC_NBT_TAG_COMPOUND ||
            p->payload.list.length < 1 || p->payload.list.length > 4096)
            goto done;
        unsigned count = (unsigned)p->payload.list.length;
        if (count == 1) {
            if (data && (data->type != MC_NBT_TAG_LONG_ARRAY || data->payload.long_array.length))
                goto done;
            continue;
        }
        unsigned bits = 0;
        for (unsigned v = count - 1; v; v >>= 1)
            bits++;
        unsigned vanilla = bits < 4 ? 4 : bits, per = 64 / vanilla;
        size_t padded = (4096 + per - 1) / per, compact = mc_packed_compact_long_count(4096, (int)bits),
               old = inverse ? compact : padded, next_n = inverse ? padded : compact;
        if (!data || data->type != MC_NBT_TAG_LONG_ARRAY || data->payload.long_array.length != (int32_t)old)
            goto done;
        uint32_t cells[4096];
        int64_t *next = calloc(next_n, sizeof(*next));
        if (!next) {
            rc = -1;
            goto done;
        }
        uint64_t mask = (UINT64_C(1) << vanilla) - 1;
        for (unsigned cell = 0; cell < 4096; cell++) {
            cells[cell] =
                inverse ? mc_packed_unpack_compact_u32(data->payload.long_array.data, old, cell, (int)bits)
                        : (uint32_t)(((uint64_t)data->payload.long_array.data[cell / per] >>
                                      ((cell % per) * vanilla)) &
                                     mask);
            if (cells[cell] >= count) {
                free(next);
                goto done;
            }
            if (inverse)
                next[cell / per] = (int64_t)((uint64_t)next[cell / per] |
                                             ((uint64_t)cells[cell] << ((cell % per) * vanilla)));
        }
        if (!inverse) {
            /* Prove discarded padding is zero, including the final partial word. */
            for (size_t word = 0; word < old; word++) {
                unsigned entries = (unsigned)(4096 - word * per);
                if (entries > per)
                    entries = per;
                unsigned used_bits = entries * vanilla;
                if (used_bits < 64 && ((uint64_t)data->payload.long_array.data[word] >> used_bits)) {
                    free(next);
                    goto done;
                }
            }
            free(next);
            next = NULL;
            int32_t packed_n = 0;
            if (mc_packed_pack_compact_u32(cells, 4096, (int)bits, &next, &packed_n) ||
                packed_n != (int32_t)next_n) {
                free(next);
                rc = -1;
                goto done;
            }
        } else if (compact * 64 > 4096 * bits &&
                   ((uint64_t)data->payload.long_array.data[compact - 1] >> ((4096 * bits) % 64))) {
            free(next);
            goto done;
        }
        free(data->payload.long_array.data);
        data->payload.long_array.data = next;
        data->payload.long_array.length = (int32_t)next_n;
    }
    if (mc_nbt_write_named_root(root, out, n)) {
        rc = -1;
        goto done;
    }
    if (!*n || *n > G1_MAX_RAW) {
        free(*out);
        *out = NULL;
        rc = -1;
        goto done;
    }
    rc = 0;
done:
    mc_nbt_free(root);
    if (rc < 0)
        errno = EBADMSG;
    return rc;
}
