#include "mc_nbt.h"

#include <stdlib.h>
#include <string.h>

#define MC_NBT_MAX_DEPTH 512

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} mc_nbt_reader_t;

typedef struct {
    void *ctx;
    void *(*alloc)(void *ctx, size_t size);
} mc_nbt_allocator_t;

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} mc_nbt_writer_t;

static void *heap_alloc_adapter(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}

static void *arena_alloc_adapter(void *ctx, size_t size) {
    return mc_arena_alloc((mc_arena_t *)ctx, size);
}

static void *alloc_zero(const mc_nbt_allocator_t *alloc, size_t size) {
    void *p;

    if (!alloc || !alloc->alloc) return NULL;
    if (size == 0) size = 1;
    p = alloc->alloc(alloc->ctx, size);
    if (!p) return NULL;
    memset(p, 0, size);
    return p;
}

static char *alloc_string_copy(const mc_nbt_allocator_t *alloc, const uint8_t *src, size_t len) {
    char *s;

    if (!alloc) return NULL;
    s = (char *)alloc_zero(alloc, len + 1);
    if (!s) return NULL;
    if (len > 0) memcpy(s, src, len);
    s[len] = '\0';
    return s;
}

static int r_u8(mc_nbt_reader_t *r, uint8_t *out) {
    if (!r || !out) return -1;
    if (r->pos + 1 > r->len) return -1;
    *out = r->data[r->pos++];
    return 0;
}

static int r_i8(mc_nbt_reader_t *r, int8_t *out) {
    uint8_t v = 0;
    if (r_u8(r, &v) != 0) return -1;
    *out = (int8_t)v;
    return 0;
}

static int r_i16(mc_nbt_reader_t *r, int16_t *out) {
    if (!r || !out) return -1;
    if (r->pos + 2 > r->len) return -1;
    *out = (int16_t)(((uint16_t)r->data[r->pos] << 8) | (uint16_t)r->data[r->pos + 1]);
    r->pos += 2;
    return 0;
}

static int r_i32(mc_nbt_reader_t *r, int32_t *out) {
    uint32_t v = 0;

    if (!r || !out) return -1;
    if (r->pos + 4 > r->len) return -1;
    v |= (uint32_t)r->data[r->pos + 0] << 24;
    v |= (uint32_t)r->data[r->pos + 1] << 16;
    v |= (uint32_t)r->data[r->pos + 2] << 8;
    v |= (uint32_t)r->data[r->pos + 3];
    r->pos += 4;
    *out = (int32_t)v;
    return 0;
}

static int r_i64(mc_nbt_reader_t *r, int64_t *out) {
    uint64_t v = 0;

    if (!r || !out) return -1;
    if (r->pos + 8 > r->len) return -1;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | r->data[r->pos + (size_t)i];
    }
    r->pos += 8;
    *out = (int64_t)v;
    return 0;
}

static int r_f32(mc_nbt_reader_t *r, float *out) {
    uint32_t u = 0;

    if (!r || !out) return -1;
    if (r->pos + 4 > r->len) return -1;
    u |= (uint32_t)r->data[r->pos + 0] << 24;
    u |= (uint32_t)r->data[r->pos + 1] << 16;
    u |= (uint32_t)r->data[r->pos + 2] << 8;
    u |= (uint32_t)r->data[r->pos + 3];
    r->pos += 4;
    memcpy(out, &u, sizeof(u));
    return 0;
}

static int r_f64(mc_nbt_reader_t *r, double *out) {
    uint64_t u = 0;

    if (!r || !out) return -1;
    if (r->pos + 8 > r->len) return -1;
    for (int i = 0; i < 8; i++) {
        u = (u << 8) | r->data[r->pos + (size_t)i];
    }
    r->pos += 8;
    memcpy(out, &u, sizeof(u));
    return 0;
}

static int r_string(mc_nbt_reader_t *r, const mc_nbt_allocator_t *alloc, char **out) {
    uint16_t n = 0;

    if (!r || !alloc || !out) return -1;
    if (r->pos + 2 > r->len) return -1;
    n = (uint16_t)(((uint16_t)r->data[r->pos] << 8) | (uint16_t)r->data[r->pos + 1]);
    r->pos += 2;
    if (r->pos + n > r->len) return -1;

    *out = alloc_string_copy(alloc, r->data + r->pos, n);
    if (!*out) return -1;
    r->pos += n;
    return 0;
}

static const char *type_name(mc_nbt_type_t t) {
    switch (t) {
        case MC_NBT_TAG_END: return "END";
        case MC_NBT_TAG_BYTE: return "BYTE";
        case MC_NBT_TAG_SHORT: return "SHORT";
        case MC_NBT_TAG_INT: return "INT";
        case MC_NBT_TAG_LONG: return "LONG";
        case MC_NBT_TAG_FLOAT: return "FLOAT";
        case MC_NBT_TAG_DOUBLE: return "DOUBLE";
        case MC_NBT_TAG_BYTE_ARRAY: return "BYTE_ARRAY";
        case MC_NBT_TAG_STRING: return "STRING";
        case MC_NBT_TAG_LIST: return "LIST";
        case MC_NBT_TAG_COMPOUND: return "COMPOUND";
        case MC_NBT_TAG_INT_ARRAY: return "INT_ARRAY";
        case MC_NBT_TAG_LONG_ARRAY: return "LONG_ARRAY";
        default: return "UNKNOWN";
    }
}

static int read_payload(mc_nbt_reader_t *r, const mc_nbt_allocator_t *alloc, mc_nbt_tag_t *tag, int depth);

static int read_list(mc_nbt_reader_t *r, const mc_nbt_allocator_t *alloc, mc_nbt_tag_t *tag, int depth) {
    uint8_t elem_type_u8 = 0;
    int32_t length = 0;
    mc_nbt_tag_t **items = NULL;

    if (r_u8(r, &elem_type_u8) != 0) return -1;
    if (r_i32(r, &length) != 0) return -1;
    if (length < 0) return -1;

    tag->payload.list.elem_type = (mc_nbt_type_t)elem_type_u8;
    tag->payload.list.length = length;
    if (length == 0) return 0;
    if (tag->payload.list.elem_type == MC_NBT_TAG_END) return -1;

    if ((size_t)length > SIZE_MAX / sizeof(*items)) return -1;
    items = (mc_nbt_tag_t **)alloc_zero(alloc, (size_t)length * sizeof(*items));
    if (!items) return -1;

    for (int32_t i = 0; i < length; i++) {
        mc_nbt_tag_t *item = (mc_nbt_tag_t *)alloc_zero(alloc, sizeof(*item));
        if (!item) return -1;
        item->type = tag->payload.list.elem_type;
        if (read_payload(r, alloc, item, depth + 1) != 0) return -1;
        items[i] = item;
    }

    tag->payload.list.items = items;
    return 0;
}

static int read_compound(mc_nbt_reader_t *r, const mc_nbt_allocator_t *alloc, mc_nbt_tag_t *tag, int depth) {
    int32_t count = 0;
    int32_t cap = 0;
    mc_nbt_tag_t **children = NULL;

    for (;;) {
        uint8_t type_u8 = 0;
        mc_nbt_tag_t *child = NULL;

        if (r_u8(r, &type_u8) != 0) return -1;
        if (type_u8 == MC_NBT_TAG_END) break;

        child = (mc_nbt_tag_t *)alloc_zero(alloc, sizeof(*child));
        if (!child) return -1;
        child->type = (mc_nbt_type_t)type_u8;

        if (r_string(r, alloc, &child->name) != 0) return -1;
        if (read_payload(r, alloc, child, depth + 1) != 0) return -1;

        if (count == cap) {
            int32_t new_cap = cap ? cap * 2 : 16;
            mc_nbt_tag_t **next;

            if (new_cap < cap) return -1;
            if ((size_t)new_cap > SIZE_MAX / sizeof(*next)) return -1;
            next = (mc_nbt_tag_t **)alloc_zero(alloc, (size_t)new_cap * sizeof(*next));
            if (!next) return -1;
            if (children && count > 0) memcpy(next, children, (size_t)count * sizeof(*next));
            children = next;
            cap = new_cap;
        }

        children[count++] = child;
    }

    tag->payload.compound.length = count;
    tag->payload.compound.children = children;
    return 0;
}

static int read_payload(mc_nbt_reader_t *r, const mc_nbt_allocator_t *alloc, mc_nbt_tag_t *tag, int depth) {
    if (!r || !alloc || !tag) return -1;
    if (depth > MC_NBT_MAX_DEPTH) return -1;

    switch (tag->type) {
        case MC_NBT_TAG_END:
            return -1;
        case MC_NBT_TAG_BYTE:
            return r_i8(r, &tag->payload.byte_val);
        case MC_NBT_TAG_SHORT:
            return r_i16(r, &tag->payload.short_val);
        case MC_NBT_TAG_INT:
            return r_i32(r, &tag->payload.int_val);
        case MC_NBT_TAG_LONG:
            return r_i64(r, &tag->payload.long_val);
        case MC_NBT_TAG_FLOAT:
            return r_f32(r, &tag->payload.float_val);
        case MC_NBT_TAG_DOUBLE:
            return r_f64(r, &tag->payload.double_val);
        case MC_NBT_TAG_STRING:
            return r_string(r, alloc, &tag->payload.string_val);

        case MC_NBT_TAG_BYTE_ARRAY: {
            int32_t n = 0;
            int8_t *p = NULL;

            if (r_i32(r, &n) != 0) return -1;
            if (n < 0) return -1;
            tag->payload.byte_array.length = n;
            if (n == 0) return 0;
            if (r->pos + (size_t)n > r->len) return -1;

            p = (int8_t *)alloc_zero(alloc, (size_t)n);
            if (!p) return -1;
            memcpy(p, r->data + r->pos, (size_t)n);
            r->pos += (size_t)n;
            tag->payload.byte_array.data = p;
            return 0;
        }

        case MC_NBT_TAG_INT_ARRAY: {
            int32_t n = 0;
            int32_t *p = NULL;

            if (r_i32(r, &n) != 0) return -1;
            if (n < 0) return -1;
            tag->payload.int_array.length = n;
            if (n == 0) return 0;
            if ((size_t)n > SIZE_MAX / sizeof(*p)) return -1;

            p = (int32_t *)alloc_zero(alloc, (size_t)n * sizeof(*p));
            if (!p) return -1;
            for (int32_t i = 0; i < n; i++) {
                if (r_i32(r, &p[i]) != 0) return -1;
            }
            tag->payload.int_array.data = p;
            return 0;
        }

        case MC_NBT_TAG_LONG_ARRAY: {
            int32_t n = 0;
            int64_t *p = NULL;

            if (r_i32(r, &n) != 0) return -1;
            if (n < 0) return -1;
            tag->payload.long_array.length = n;
            if (n == 0) return 0;
            if ((size_t)n > SIZE_MAX / sizeof(*p)) return -1;

            p = (int64_t *)alloc_zero(alloc, (size_t)n * sizeof(*p));
            if (!p) return -1;
            for (int32_t i = 0; i < n; i++) {
                if (r_i64(r, &p[i]) != 0) return -1;
            }
            tag->payload.long_array.data = p;
            return 0;
        }

        case MC_NBT_TAG_LIST:
            return read_list(r, alloc, tag, depth);

        case MC_NBT_TAG_COMPOUND:
            return read_compound(r, alloc, tag, depth);

        default:
            return -1;
    }
}

static int read_root(mc_nbt_reader_t *r, const mc_nbt_allocator_t *alloc, bool named_root, mc_nbt_tag_t **out,
                     size_t *bytes_read) {
    uint8_t type_u8 = 0;
    mc_nbt_tag_t *root = NULL;

    if (!r || !alloc || !out) return -1;
    if (r_u8(r, &type_u8) != 0) return -1;
    if (type_u8 == MC_NBT_TAG_END) return -1;

    root = (mc_nbt_tag_t *)alloc_zero(alloc, sizeof(*root));
    if (!root) return -1;
    root->type = (mc_nbt_type_t)type_u8;

    if (named_root && r_string(r, alloc, &root->name) != 0) return -1;
    if (read_payload(r, alloc, root, 0) != 0) return -1;

    if (bytes_read) *bytes_read = r->pos;
    *out = root;
    return 0;
}

int mc_nbt_read_named_root(const uint8_t *data, size_t len, mc_nbt_tag_t **out, size_t *bytes_read) {
    mc_nbt_reader_t r = {data, len, 0};
    mc_nbt_allocator_t alloc = {NULL, heap_alloc_adapter};

    if (!data || !out) return -1;
    return read_root(&r, &alloc, true, out, bytes_read);
}

int mc_nbt_read_unnamed_root(const uint8_t *data, size_t len, mc_nbt_tag_t **out, size_t *bytes_read) {
    mc_nbt_reader_t r = {data, len, 0};
    mc_nbt_allocator_t alloc = {NULL, heap_alloc_adapter};

    if (!data || !out) return -1;
    return read_root(&r, &alloc, false, out, bytes_read);
}

int mc_nbt_read_named_root_arena(const uint8_t *data, size_t len, mc_arena_t *arena, mc_nbt_tag_t **out, size_t *bytes_read) {
    mc_nbt_reader_t r = {data, len, 0};
    mc_nbt_allocator_t alloc = {arena, arena_alloc_adapter};

    if (!data || !arena || !out) return -1;
    return read_root(&r, &alloc, true, out, bytes_read);
}

int mc_nbt_read_unnamed_root_arena(const uint8_t *data, size_t len, mc_arena_t *arena, mc_nbt_tag_t **out, size_t *bytes_read) {
    mc_nbt_reader_t r = {data, len, 0};
    mc_nbt_allocator_t alloc = {arena, arena_alloc_adapter};

    if (!data || !arena || !out) return -1;
    return read_root(&r, &alloc, false, out, bytes_read);
}

static int w_reserve(mc_nbt_writer_t *w, size_t extra) {
    size_t need;
    size_t new_cap;
    uint8_t *next;

    if (!w) return -1;
    if (extra > SIZE_MAX - w->len) return -1;
    need = w->len + extra;
    if (need <= w->cap) return 0;

    new_cap = w->cap ? w->cap : 256;
    while (new_cap < need) {
        if (new_cap > SIZE_MAX / 2) return -1;
        new_cap *= 2;
    }

    next = (uint8_t *)realloc(w->data, new_cap);
    if (!next) return -1;
    w->data = next;
    w->cap = new_cap;
    return 0;
}

static int w_u8(mc_nbt_writer_t *w, uint8_t v) {
    if (w_reserve(w, 1) != 0) return -1;
    w->data[w->len++] = v;
    return 0;
}

static int w_i8(mc_nbt_writer_t *w, int8_t v) {
    return w_u8(w, (uint8_t)v);
}

static int w_i16(mc_nbt_writer_t *w, int16_t v) {
    uint16_t u = (uint16_t)v;

    if (w_reserve(w, 2) != 0) return -1;
    w->data[w->len + 0] = (uint8_t)((u >> 8) & 0xFF);
    w->data[w->len + 1] = (uint8_t)(u & 0xFF);
    w->len += 2;
    return 0;
}

static int w_i32(mc_nbt_writer_t *w, int32_t v) {
    uint32_t u = (uint32_t)v;

    if (w_reserve(w, 4) != 0) return -1;
    w->data[w->len + 0] = (uint8_t)((u >> 24) & 0xFF);
    w->data[w->len + 1] = (uint8_t)((u >> 16) & 0xFF);
    w->data[w->len + 2] = (uint8_t)((u >> 8) & 0xFF);
    w->data[w->len + 3] = (uint8_t)(u & 0xFF);
    w->len += 4;
    return 0;
}

static int w_i64(mc_nbt_writer_t *w, int64_t v) {
    uint64_t u = (uint64_t)v;

    if (w_reserve(w, 8) != 0) return -1;
    for (int i = 0; i < 8; i++) {
        w->data[w->len + (size_t)i] = (uint8_t)((u >> (56 - i * 8)) & 0xFF);
    }
    w->len += 8;
    return 0;
}

static int w_f32(mc_nbt_writer_t *w, float v) {
    uint32_t u = 0;
    memcpy(&u, &v, sizeof(u));
    return w_i32(w, (int32_t)u);
}

static int w_f64(mc_nbt_writer_t *w, double v) {
    uint64_t u = 0;
    memcpy(&u, &v, sizeof(u));
    return w_i64(w, (int64_t)u);
}

static int w_bytes(mc_nbt_writer_t *w, const void *p, size_t n) {
    if (n == 0) return 0;
    if (w_reserve(w, n) != 0) return -1;
    memcpy(w->data + w->len, p, n);
    w->len += n;
    return 0;
}

static int w_string(mc_nbt_writer_t *w, const char *s) {
    size_t n;

    if (!s) s = "";
    n = strlen(s);
    if (n > 0xFFFFu) return -1;
    if (w_i16(w, (int16_t)n) != 0) return -1;
    return w_bytes(w, s, n);
}

static int write_payload(mc_nbt_writer_t *w, const mc_nbt_tag_t *tag, int depth);

static int write_list(mc_nbt_writer_t *w, const mc_nbt_tag_t *tag, int depth) {
    if (w_u8(w, (uint8_t)tag->payload.list.elem_type) != 0) return -1;
    if (w_i32(w, tag->payload.list.length) != 0) return -1;
    if (tag->payload.list.length <= 0) return 0;
    if (!tag->payload.list.items) return -1;

    for (int32_t i = 0; i < tag->payload.list.length; i++) {
        const mc_nbt_tag_t *it = tag->payload.list.items[i];
        if (!it || it->type != tag->payload.list.elem_type) return -1;
        if (write_payload(w, it, depth + 1) != 0) return -1;
    }
    return 0;
}

static int write_compound(mc_nbt_writer_t *w, const mc_nbt_tag_t *tag, int depth) {
    for (int32_t i = 0; i < tag->payload.compound.length; i++) {
        const mc_nbt_tag_t *ch = tag->payload.compound.children ? tag->payload.compound.children[i] : NULL;
        if (!ch) continue;
        if (!ch->name || ch->type == MC_NBT_TAG_END) return -1;
        if (w_u8(w, (uint8_t)ch->type) != 0) return -1;
        if (w_string(w, ch->name) != 0) return -1;
        if (write_payload(w, ch, depth + 1) != 0) return -1;
    }
    return w_u8(w, 0x00);
}

static int write_payload(mc_nbt_writer_t *w, const mc_nbt_tag_t *tag, int depth) {
    if (!w || !tag) return -1;
    if (depth > MC_NBT_MAX_DEPTH) return -1;

    switch (tag->type) {
        case MC_NBT_TAG_END:
            return -1;
        case MC_NBT_TAG_BYTE:
            return w_i8(w, tag->payload.byte_val);
        case MC_NBT_TAG_SHORT:
            return w_i16(w, tag->payload.short_val);
        case MC_NBT_TAG_INT:
            return w_i32(w, tag->payload.int_val);
        case MC_NBT_TAG_LONG:
            return w_i64(w, tag->payload.long_val);
        case MC_NBT_TAG_FLOAT:
            return w_f32(w, tag->payload.float_val);
        case MC_NBT_TAG_DOUBLE:
            return w_f64(w, tag->payload.double_val);
        case MC_NBT_TAG_STRING:
            return w_string(w, tag->payload.string_val);

        case MC_NBT_TAG_BYTE_ARRAY: {
            int32_t n = tag->payload.byte_array.length;
            if (n < 0) return -1;
            if (w_i32(w, n) != 0) return -1;
            if (n == 0) return 0;
            if (!tag->payload.byte_array.data) return -1;
            return w_bytes(w, tag->payload.byte_array.data, (size_t)n);
        }

        case MC_NBT_TAG_INT_ARRAY: {
            int32_t n = tag->payload.int_array.length;
            if (n < 0) return -1;
            if (w_i32(w, n) != 0) return -1;
            if (n == 0) return 0;
            if (!tag->payload.int_array.data) return -1;
            for (int32_t i = 0; i < n; i++) {
                if (w_i32(w, tag->payload.int_array.data[i]) != 0) return -1;
            }
            return 0;
        }

        case MC_NBT_TAG_LONG_ARRAY: {
            int32_t n = tag->payload.long_array.length;
            if (n < 0) return -1;
            if (w_i32(w, n) != 0) return -1;
            if (n == 0) return 0;
            if (!tag->payload.long_array.data) return -1;
            for (int32_t i = 0; i < n; i++) {
                if (w_i64(w, tag->payload.long_array.data[i]) != 0) return -1;
            }
            return 0;
        }

        case MC_NBT_TAG_LIST:
            return write_list(w, tag, depth);
        case MC_NBT_TAG_COMPOUND:
            return write_compound(w, tag, depth);
        default:
            return -1;
    }
}

static int write_root(mc_nbt_writer_t *w, bool named_root, const mc_nbt_tag_t *root) {
    if (!w || !root || root->type == MC_NBT_TAG_END) return -1;
    if (w_u8(w, (uint8_t)root->type) != 0) return -1;
    if (named_root && w_string(w, root->name ? root->name : "") != 0) return -1;
    return write_payload(w, root, 0);
}

static int nbt_write_common(bool named_root, const mc_nbt_tag_t *root, uint8_t **out, size_t *out_len) {
    mc_nbt_writer_t w = {0};
    uint8_t *shrunk = NULL;

    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!root || !out || !out_len) return -1;

    if (write_root(&w, named_root, root) != 0) {
        free(w.data);
        return -1;
    }

    shrunk = (uint8_t *)realloc(w.data, w.len ? w.len : 1);
    if (shrunk) w.data = shrunk;
    *out = w.data;
    *out_len = w.len;
    return 0;
}

int mc_nbt_write_named_root(const mc_nbt_tag_t *root, uint8_t **out, size_t *out_len) {
    return nbt_write_common(true, root, out, out_len);
}

int mc_nbt_write_unnamed_root(const mc_nbt_tag_t *root, uint8_t **out, size_t *out_len) {
    return nbt_write_common(false, root, out, out_len);
}

void mc_nbt_free(mc_nbt_tag_t *tag) {
    if (!tag) return;

    free(tag->name);
    tag->name = NULL;

    switch (tag->type) {
        case MC_NBT_TAG_STRING:
            free(tag->payload.string_val);
            break;
        case MC_NBT_TAG_BYTE_ARRAY:
            free(tag->payload.byte_array.data);
            break;
        case MC_NBT_TAG_INT_ARRAY:
            free(tag->payload.int_array.data);
            break;
        case MC_NBT_TAG_LONG_ARRAY:
            free(tag->payload.long_array.data);
            break;
        case MC_NBT_TAG_LIST:
            if (tag->payload.list.items) {
                for (int32_t i = 0; i < tag->payload.list.length; i++) {
                    mc_nbt_free(tag->payload.list.items[i]);
                }
            }
            free(tag->payload.list.items);
            break;
        case MC_NBT_TAG_COMPOUND:
            if (tag->payload.compound.children) {
                for (int32_t i = 0; i < tag->payload.compound.length; i++) {
                    mc_nbt_free(tag->payload.compound.children[i]);
                }
            }
            free(tag->payload.compound.children);
            break;
        default:
            break;
    }

    free(tag);
}

const mc_nbt_tag_t *mc_nbt_compound_get(const mc_nbt_tag_t *compound, const char *name) {
    if (!compound || compound->type != MC_NBT_TAG_COMPOUND || !name) return NULL;
    for (int32_t i = 0; i < compound->payload.compound.length; i++) {
        mc_nbt_tag_t *child = compound->payload.compound.children[i];
        if (!child || !child->name) continue;
        if (strcmp(child->name, name) == 0) return child;
    }
    return NULL;
}

bool mc_anvil_validate_chunk(const mc_nbt_tag_t *root) {
    const mc_nbt_tag_t *version = NULL;

    if (!root || root->type != MC_NBT_TAG_COMPOUND) return false;
    version = mc_nbt_compound_get(root, "DataVersion");
    if (!version || version->type != MC_NBT_TAG_INT) return false;
    return version->payload.int_val == MC_ANVIL_EXPECTED_DATA_VERSION;
}

static void dump_indent(FILE *out, int indent) {
    for (int i = 0; i < indent; i++) fputc(' ', out);
}

void mc_nbt_dump(const mc_nbt_tag_t *tag, FILE *out, int indent, int max_depth) {
    if (!tag || !out) return;
    if (max_depth < 0) return;

    dump_indent(out, indent);
    if (tag->name) fprintf(out, "%s(\"%s\")", type_name(tag->type), tag->name);
    else fprintf(out, "%s", type_name(tag->type));

    switch (tag->type) {
        case MC_NBT_TAG_BYTE:
            fprintf(out, " = %d\n", (int)tag->payload.byte_val);
            break;
        case MC_NBT_TAG_SHORT:
            fprintf(out, " = %d\n", (int)tag->payload.short_val);
            break;
        case MC_NBT_TAG_INT:
            fprintf(out, " = %d\n", tag->payload.int_val);
            break;
        case MC_NBT_TAG_LONG:
            fprintf(out, " = %lld\n", (long long)tag->payload.long_val);
            break;
        case MC_NBT_TAG_FLOAT:
            fprintf(out, " = %f\n", tag->payload.float_val);
            break;
        case MC_NBT_TAG_DOUBLE:
            fprintf(out, " = %f\n", tag->payload.double_val);
            break;
        case MC_NBT_TAG_STRING:
            fprintf(out, " = \"%s\"\n", tag->payload.string_val ? tag->payload.string_val : "");
            break;
        case MC_NBT_TAG_BYTE_ARRAY:
            fprintf(out, " (len=%d)\n", tag->payload.byte_array.length);
            break;
        case MC_NBT_TAG_INT_ARRAY:
            fprintf(out, " (len=%d)\n", tag->payload.int_array.length);
            break;
        case MC_NBT_TAG_LONG_ARRAY:
            fprintf(out, " (len=%d)\n", tag->payload.long_array.length);
            break;
        case MC_NBT_TAG_LIST:
            fprintf(out, " (elem=%s len=%d)\n", type_name(tag->payload.list.elem_type), tag->payload.list.length);
            if (max_depth == 0) break;
            for (int32_t i = 0; i < tag->payload.list.length; i++) {
                mc_nbt_dump(tag->payload.list.items[i], out, indent + 2, max_depth - 1);
            }
            break;
        case MC_NBT_TAG_COMPOUND:
            fprintf(out, " (len=%d)\n", tag->payload.compound.length);
            if (max_depth == 0) break;
            for (int32_t i = 0; i < tag->payload.compound.length; i++) {
                mc_nbt_dump(tag->payload.compound.children[i], out, indent + 2, max_depth - 1);
            }
            break;
        default:
            fprintf(out, "\n");
            break;
    }
}
