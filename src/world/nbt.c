#include "mc_nbt.h"

#include <stdlib.h>
#include <string.h>

#define MC_NBT_MAX_DEPTH 512

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} mc_nbt_reader_t;

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
    uint16_t v = (uint16_t)((r->data[r->pos] << 8) | r->data[r->pos + 1]);
    r->pos += 2;
    *out = (int16_t)v;
    return 0;
}

static int r_i32(mc_nbt_reader_t *r, int32_t *out) {
    if (!r || !out) return -1;
    if (r->pos + 4 > r->len) return -1;
    uint32_t v = 0;
    v |= (uint32_t)r->data[r->pos + 0] << 24;
    v |= (uint32_t)r->data[r->pos + 1] << 16;
    v |= (uint32_t)r->data[r->pos + 2] << 8;
    v |= (uint32_t)r->data[r->pos + 3];
    r->pos += 4;
    *out = (int32_t)v;
    return 0;
}

static int r_i64(mc_nbt_reader_t *r, int64_t *out) {
    if (!r || !out) return -1;
    if (r->pos + 8 > r->len) return -1;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | r->data[r->pos + (size_t)i];
    }
    r->pos += 8;
    *out = (int64_t)v;
    return 0;
}

static int r_f32(mc_nbt_reader_t *r, float *out) {
    if (!r || !out) return -1;
    uint32_t u = 0;
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
    if (!r || !out) return -1;
    if (r->pos + 8 > r->len) return -1;
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) {
        u = (u << 8) | r->data[r->pos + (size_t)i];
    }
    r->pos += 8;
    memcpy(out, &u, sizeof(u));
    return 0;
}

static int r_string(mc_nbt_reader_t *r, char **out) {
    if (!r || !out) return -1;
    if (r->pos + 2 > r->len) return -1;
    uint16_t n = (uint16_t)((r->data[r->pos] << 8) | r->data[r->pos + 1]);
    r->pos += 2;
    if (r->pos + n > r->len) return -1;

    char *s = (char *)malloc((size_t)n + 1);
    if (!s) return -1;
    memcpy(s, r->data + r->pos, n);
    s[n] = '\0';
    r->pos += n;
    *out = s;
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

static int read_payload(mc_nbt_reader_t *r, mc_nbt_tag_t *tag, int depth);

static int read_list(mc_nbt_reader_t *r, mc_nbt_tag_t *tag, int depth) {
    uint8_t elem_type_u8 = 0;
    int32_t length = 0;
    if (r_u8(r, &elem_type_u8) != 0) return -1;
    if (r_i32(r, &length) != 0) return -1;
    if (length < 0) return -1;

    tag->payload.list.elem_type = (mc_nbt_type_t)elem_type_u8;
    tag->payload.list.length = length;

    if (length == 0) {
        tag->payload.list.items = NULL;
        return 0;
    }

    if (tag->payload.list.elem_type == MC_NBT_TAG_END) return -1;

    size_t count = (size_t)length;
    if (count > SIZE_MAX / sizeof(mc_nbt_tag_t *)) return -1;
    mc_nbt_tag_t **items = (mc_nbt_tag_t **)calloc(count, sizeof(*items));
    if (!items) return -1;

    for (int32_t i = 0; i < length; i++) {
        mc_nbt_tag_t *item = (mc_nbt_tag_t *)calloc(1, sizeof(*item));
        if (!item) {
            for (int32_t j = 0; j < i; j++) mc_nbt_free(items[j]);
            free(items);
            return -1;
        }
        item->type = tag->payload.list.elem_type;
        item->name = NULL;
        if (read_payload(r, item, depth + 1) != 0) {
            mc_nbt_free(item);
            for (int32_t j = 0; j < i; j++) mc_nbt_free(items[j]);
            free(items);
            return -1;
        }
        items[i] = item;
    }

    tag->payload.list.items = items;
    return 0;
}

static int read_compound(mc_nbt_reader_t *r, mc_nbt_tag_t *tag, int depth) {
    int32_t count = 0;
    int32_t cap = 0;
    mc_nbt_tag_t **children = NULL;

    for (;;) {
        uint8_t type_u8 = 0;
        if (r_u8(r, &type_u8) != 0) {
            for (int32_t i = 0; i < count; i++) mc_nbt_free(children[i]);
            free(children);
            return -1;
        }

        if (type_u8 == MC_NBT_TAG_END) break;

        mc_nbt_tag_t *child = (mc_nbt_tag_t *)calloc(1, sizeof(*child));
        if (!child) {
            for (int32_t i = 0; i < count; i++) mc_nbt_free(children[i]);
            free(children);
            return -1;
        }
        child->type = (mc_nbt_type_t)type_u8;

        if (r_string(r, &child->name) != 0) {
            mc_nbt_free(child);
            for (int32_t i = 0; i < count; i++) mc_nbt_free(children[i]);
            free(children);
            return -1;
        }
        if (read_payload(r, child, depth + 1) != 0) {
            mc_nbt_free(child);
            for (int32_t i = 0; i < count; i++) mc_nbt_free(children[i]);
            free(children);
            return -1;
        }

        if (count == cap) {
            int32_t new_cap = cap ? cap * 2 : 16;
            if (new_cap < cap) {
                mc_nbt_free(child);
                for (int32_t i = 0; i < count; i++) mc_nbt_free(children[i]);
                free(children);
                return -1;
            }
            size_t bytes = (size_t)new_cap * sizeof(*children);
            mc_nbt_tag_t **next = (mc_nbt_tag_t **)realloc(children, bytes);
            if (!next) {
                mc_nbt_free(child);
                for (int32_t i = 0; i < count; i++) mc_nbt_free(children[i]);
                free(children);
                return -1;
            }
            children = next;
            cap = new_cap;
        }
        children[count++] = child;
    }

    if (count == 0) {
        free(children);
        tag->payload.compound.children = NULL;
        tag->payload.compound.length = 0;
        return 0;
    }

    tag->payload.compound.children = children;
    tag->payload.compound.length = count;
    return 0;
}

static int read_payload(mc_nbt_reader_t *r, mc_nbt_tag_t *tag, int depth) {
    if (!r || !tag) return -1;
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
            return r_string(r, &tag->payload.string_val);

        case MC_NBT_TAG_BYTE_ARRAY: {
            int32_t n = 0;
            if (r_i32(r, &n) != 0) return -1;
            if (n < 0) return -1;
            tag->payload.byte_array.length = n;
            if (n == 0) {
                tag->payload.byte_array.data = NULL;
                return 0;
            }
            if (r->pos + (size_t)n > r->len) return -1;
            int8_t *p = (int8_t *)malloc((size_t)n);
            if (!p) return -1;
            memcpy(p, r->data + r->pos, (size_t)n);
            r->pos += (size_t)n;
            tag->payload.byte_array.data = p;
            return 0;
        }

        case MC_NBT_TAG_INT_ARRAY: {
            int32_t n = 0;
            if (r_i32(r, &n) != 0) return -1;
            if (n < 0) return -1;
            tag->payload.int_array.length = n;
            if (n == 0) {
                tag->payload.int_array.data = NULL;
                return 0;
            }
            size_t count = (size_t)n;
            if (count > SIZE_MAX / sizeof(int32_t)) return -1;
            int32_t *p = (int32_t *)malloc(count * sizeof(*p));
            if (!p) return -1;
            for (int32_t i = 0; i < n; i++) {
                if (r_i32(r, &p[i]) != 0) {
                    free(p);
                    return -1;
                }
            }
            tag->payload.int_array.data = p;
            return 0;
        }

        case MC_NBT_TAG_LONG_ARRAY: {
            int32_t n = 0;
            if (r_i32(r, &n) != 0) return -1;
            if (n < 0) return -1;
            tag->payload.long_array.length = n;
            if (n == 0) {
                tag->payload.long_array.data = NULL;
                return 0;
            }
            size_t count = (size_t)n;
            if (count > SIZE_MAX / sizeof(int64_t)) return -1;
            int64_t *p = (int64_t *)malloc(count * sizeof(*p));
            if (!p) return -1;
            for (int32_t i = 0; i < n; i++) {
                if (r_i64(r, &p[i]) != 0) {
                    free(p);
                    return -1;
                }
            }
            tag->payload.long_array.data = p;
            return 0;
        }

        case MC_NBT_TAG_LIST:
            return read_list(r, tag, depth);

        case MC_NBT_TAG_COMPOUND:
            return read_compound(r, tag, depth);

        default:
            return -1;
    }
}

static int read_root(mc_nbt_reader_t *r, bool named_root, mc_nbt_tag_t **out, size_t *bytes_read) {
    if (!r || !out) return -1;
    uint8_t type_u8 = 0;
    if (r_u8(r, &type_u8) != 0) return -1;
    if (type_u8 == MC_NBT_TAG_END) return -1;

    mc_nbt_tag_t *root = (mc_nbt_tag_t *)calloc(1, sizeof(*root));
    if (!root) return -1;
    root->type = (mc_nbt_type_t)type_u8;

    if (named_root) {
        if (r_string(r, &root->name) != 0) {
            mc_nbt_free(root);
            return -1;
        }
    }

    if (read_payload(r, root, 0) != 0) {
        mc_nbt_free(root);
        return -1;
    }

    if (bytes_read) *bytes_read = r->pos;
    *out = root;
    return 0;
}

int mc_nbt_read_named_root(const uint8_t *data, size_t len, mc_nbt_tag_t **out, size_t *bytes_read) {
    if (!data || !out) return -1;
    mc_nbt_reader_t r = {data, len, 0};
    return read_root(&r, true, out, bytes_read);
}

int mc_nbt_read_unnamed_root(const uint8_t *data, size_t len, mc_nbt_tag_t **out, size_t *bytes_read) {
    if (!data || !out) return -1;
    mc_nbt_reader_t r = {data, len, 0};
    return read_root(&r, false, out, bytes_read);
}

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} mc_nbt_writer_t;

static int w_reserve(mc_nbt_writer_t *w, size_t extra) {
    if (!w) return -1;
    if (extra > SIZE_MAX - w->len) return -1;
    size_t need = w->len + extra;
    if (need <= w->cap) return 0;
    size_t new_cap = w->cap ? w->cap : 256;
    while (new_cap < need) {
        if (new_cap > SIZE_MAX / 2) return -1;
        new_cap *= 2;
    }
    uint8_t *next = (uint8_t *)realloc(w->data, new_cap);
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
    if (w_reserve(w, 2) != 0) return -1;
    uint16_t u = (uint16_t)v;
    w->data[w->len + 0] = (uint8_t)((u >> 8) & 0xFF);
    w->data[w->len + 1] = (uint8_t)(u & 0xFF);
    w->len += 2;
    return 0;
}

static int w_i32(mc_nbt_writer_t *w, int32_t v) {
    if (w_reserve(w, 4) != 0) return -1;
    uint32_t u = (uint32_t)v;
    w->data[w->len + 0] = (uint8_t)((u >> 24) & 0xFF);
    w->data[w->len + 1] = (uint8_t)((u >> 16) & 0xFF);
    w->data[w->len + 2] = (uint8_t)((u >> 8) & 0xFF);
    w->data[w->len + 3] = (uint8_t)(u & 0xFF);
    w->len += 4;
    return 0;
}

static int w_i64(mc_nbt_writer_t *w, int64_t v) {
    if (w_reserve(w, 8) != 0) return -1;
    uint64_t u = (uint64_t)v;
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
    if (!s) s = "";
    size_t n = strlen(s);
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
        if (!it) return -1;
        if (it->type != tag->payload.list.elem_type) return -1;
        if (write_payload(w, it, depth + 1) != 0) return -1;
    }
    return 0;
}

static int write_compound(mc_nbt_writer_t *w, const mc_nbt_tag_t *tag, int depth) {
    for (int32_t i = 0; i < tag->payload.compound.length; i++) {
        const mc_nbt_tag_t *ch = tag->payload.compound.children ? tag->payload.compound.children[i] : NULL;
        if (!ch) continue;
        if (!ch->name) return -1;
        if (ch->type == MC_NBT_TAG_END) return -1;
        if (w_u8(w, (uint8_t)ch->type) != 0) return -1;
        if (w_string(w, ch->name) != 0) return -1;
        if (write_payload(w, ch, depth + 1) != 0) return -1;
    }
    return w_u8(w, 0x00); /* TAG_End */
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
    if (!w || !root) return -1;
    if (root->type == MC_NBT_TAG_END) return -1;
    if (w_u8(w, (uint8_t)root->type) != 0) return -1;
    if (named_root) {
        if (w_string(w, root->name ? root->name : "") != 0) return -1;
    }
    return write_payload(w, root, 0);
}

static int nbt_write_common(bool named_root, const mc_nbt_tag_t *root, uint8_t **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!root || !out || !out_len) return -1;

    mc_nbt_writer_t w = {0};
    if (write_root(&w, named_root, root) != 0) {
        free(w.data);
        return -1;
    }

    uint8_t *shrunk = (uint8_t *)realloc(w.data, w.len ? w.len : 1);
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

static void dump_indent(FILE *out, int indent) {
    for (int i = 0; i < indent; i++) fputc(' ', out);
}

void mc_nbt_dump(const mc_nbt_tag_t *tag, FILE *out, int indent, int max_depth) {
    if (!tag || !out) return;
    if (max_depth < 0) return;

    dump_indent(out, indent);
    if (tag->name) {
        fprintf(out, "%s(\"%s\")", type_name(tag->type), tag->name);
    } else {
        fprintf(out, "%s", type_name(tag->type));
    }

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
