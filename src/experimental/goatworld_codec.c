#include "g1_internal.h"
#include "mc_nbt.h"
#include <openssl/evp.h>
#include <zlib.h>
#include <lz4.h>
#include <zstd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
uint32_t g1_u32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
uint64_t g1_u64(const uint8_t *p) { return (uint64_t)g1_u32(p) << 32 | g1_u32(p + 4); }
void g1_p32(uint8_t *p, uint32_t n) {
    p[0] = n >> 24;
    p[1] = n >> 16;
    p[2] = n >> 8;
    p[3] = n;
}
void g1_p64(uint8_t *p, uint64_t n) {
    g1_p32(p, (uint32_t)(n >> 32));
    g1_p32(p + 4, (uint32_t)n);
}
int g1_sha(const void *data, size_t n, uint8_t out[32]) {
    unsigned bytes = 0;
    return EVP_Digest(data, n, out, &bytes, EVP_sha256(), NULL) == 1 && bytes == 32 ? 0 : -1;
}
int g1_profile(const char *name, g1_profile_t *out) {
    const char *names[] = {"none",  "zlib1",   "zlib6",     "lz4",       "zstd1",    "zstd3",
                           "zstd9", "lz4-16k", "zstd3-16k", "none-pack", "lz4-pack", "zstd3-pack"};
    const g1_profile_t profiles[] = {{0, 0, 0}, {1, 0, 1}, {1, 0, 6}, {2, 0, 0}, {3, 0, 1}, {3, 0, 3},
                                     {3, 0, 9}, {2, 1, 0}, {3, 1, 3}, {0, 2, 0}, {2, 2, 0}, {3, 2, 3}};
    for (size_t i = 0; i < sizeof(profiles) / sizeof(*profiles); i++)
        if (!strcmp(name, names[i])) {
            *out = profiles[i];
            return 0;
        }
    errno = EINVAL;
    return -1;
}
static bool valid(g1_profile_t p) {
    return p.framed <= 3 &&
           ((p.codec == 0 && p.level == 0) || (p.codec == 1 && (p.level == 1 || p.level == 6)) ||
            (p.codec == 2 && p.level == 0) ||
            (p.codec == 3 && (p.level == 1 || p.level == 3 || p.level == 9)));
}
const char *g1_codec_versions(void) {
    static char text[160];
    snprintf(text, sizeof(text), "zlib=%s lz4=%s zstd=%s sqlite=%s", zlibVersion(), LZ4_versionString(),
             ZSTD_versionString(), sqlite3_libversion());
    return text;
}
static int encode_one(const uint8_t *raw, size_t n, g1_profile_t p, uint8_t **out, size_t *bytes) {
    size_t cap = n;
    if (p.codec == 1)
        cap = compressBound((uLong)n);
    else if (p.codec == 2)
        cap = (size_t)LZ4_compressBound((int)n);
    else if (p.codec == 3)
        cap = ZSTD_compressBound(n);
    uint8_t *data = malloc(cap ? cap : 1);
    if (!data)
        return -1;
    size_t got = n;
    if (!p.codec)
        memcpy(data, raw, n);
    else if (p.codec == 1) {
        uLongf v = cap;
        if (compress2(data, &v, raw, n, p.level) != Z_OK)
            goto fail;
        got = v;
    } else if (p.codec == 2) {
        int v = LZ4_compress_default((const char *)raw, (char *)data, (int)n, (int)cap);
        if (v <= 0)
            goto fail;
        got = (size_t)v;
    } else {
        got = ZSTD_compress(data, cap, raw, n, p.level);
        if (ZSTD_isError(got))
            goto fail;
    }
    *out = data;
    *bytes = got;
    return 0;
fail:
    free(data);
    errno = EBADMSG;
    return -1;
}
static int decode_one(const uint8_t *in, size_t n, uint8_t *out, size_t raw, g1_profile_t p) {
    if (!p.codec) {
        if (n != raw)
            return -1;
        memcpy(out, in, n);
        return 0;
    }
    if (p.codec == 1) {
        z_stream z = {0};
        if (inflateInit(&z) != Z_OK)
            return -1;
        z.next_in = (Bytef *)in;
        z.avail_in = (uInt)n;
        z.next_out = out;
        z.avail_out = (uInt)raw;
        int r = inflate(&z, Z_FINISH);
        bool ok = r == Z_STREAM_END && z.total_in == n && z.total_out == raw;
        inflateEnd(&z);
        return ok ? 0 : -1;
    }
    if (p.codec == 2)
        return LZ4_decompress_safe((const char *)in, (char *)out, (int)n, (int)raw) == (int)raw ? 0 : -1;
    size_t frame = ZSTD_findFrameCompressedSize(in, n);
    if (ZSTD_isError(frame) || frame != n)
        return -1;
    size_t got = ZSTD_decompress(out, raw, in, n);
    return !ZSTD_isError(got) && got == raw ? 0 : -1;
}
int g1_encode(const uint8_t *raw, size_t n, g1_profile_t p, uint8_t **out, size_t *bytes) {
    if (!raw || !n || n > G1_MAX_RAW || !valid(p)) {
        errno = EINVAL;
        return -1;
    }
    if (p.framed & 2) {
        uint8_t *packed = NULL, *encoded = NULL;
        size_t packed_n = 0, encoded_n = 0;
        int rc = g1_pack(raw, n, false, &packed, &packed_n);
        if (rc < 0)
            return -1;
        g1_profile_t inner = p;
        inner.framed &= 1;
        if (g1_encode(rc ? raw : packed, rc ? n : packed_n, inner, &encoded, &encoded_n)) {
            free(packed);
            return -1;
        }
        uint8_t *data = malloc(encoded_n + 8);
        if (!data) {
            free(packed);
            free(encoded);
            return -1;
        }
        memcpy(data, rc ? "GPK0" : "GPK1", 4);
        g1_p32(data + 4, (uint32_t)(rc ? n : packed_n));
        memcpy(data + 8, encoded, encoded_n);
        free(packed);
        free(encoded);
        *out = data;
        *bytes = encoded_n + 8;
        return 0;
    }
    if (!p.framed)
        return encode_one(raw, n, p, out, bytes);
    size_t frames = (n + 16383) / 16384, cap = n + n / 8 + frames * 128 + 16, at = 8;
    uint8_t *data = malloc(cap);
    if (!data)
        return -1;
    g1_p32(data, 16384);
    g1_p32(data + 4, (uint32_t)frames);
    for (size_t off = 0; off < n; off += 16384) {
        size_t len = n - off;
        if (len > 16384)
            len = 16384;
        uint8_t *part = NULL;
        size_t got = 0;
        if (encode_one(raw + off, len, p, &part, &got) || at + 8 + got > cap) {
            free(part);
            free(data);
            return -1;
        }
        g1_p32(data + at, (uint32_t)len);
        g1_p32(data + at + 4, (uint32_t)got);
        memcpy(data + at + 8, part, got);
        at += 8 + got;
        free(part);
    }
    *out = data;
    *bytes = at;
    return 0;
}
int g1_decode(const uint8_t *data, size_t n, size_t raw, g1_profile_t p, uint8_t **out) {
    *out = NULL;
    if (!data || !n || n > G1_MAX_RAW * 2u || !raw || raw > G1_MAX_RAW || !valid(p)) {
        errno = EBADMSG;
        return -1;
    }
    if (p.framed & 2) {
        if (n < 9 || (memcmp(data, "GPK0", 4) && memcmp(data, "GPK1", 4)))
            goto invalid;
        uint8_t *inner = NULL;
        size_t length = g1_u32(data + 4);
        g1_profile_t q = p;
        q.framed &= 1;
        if (g1_decode(data + 8, n - 8, length, q, &inner))
            return -1;
        if (data[3] == '0') {
            if (length != raw) {
                free(inner);
                goto invalid;
            }
            *out = inner;
            return 0;
        }
        size_t got = 0;
        int rc = g1_pack(inner, length, true, out, &got);
        free(inner);
        if (rc || got != raw) {
            free(*out);
            *out = NULL;
            goto invalid;
        }
        return 0;
    }
    uint8_t *result = malloc(raw);
    if (!result)
        return -1;
    int rc = -1;
    if (!p.framed)
        rc = decode_one(data, n, result, raw, p);
    else if (n >= 8 && g1_u32(data) == 16384 && g1_u32(data + 4) == (raw + 16383) / 16384) {
        size_t at = 8, off = 0;
        rc = 0;
        while (off < raw) {
            if (at + 8 > n) {
                rc = -1;
                break;
            }
            size_t a = g1_u32(data + at), b = g1_u32(data + at + 4);
            at += 8;
            if (a != (raw - off > 16384 ? 16384 : raw - off) || b > n - at ||
                decode_one(data + at, b, result + off, a, p)) {
                rc = -1;
                break;
            }
            off += a;
            at += b;
        }
        if (at != n)
            rc = -1;
    }
    if (rc) {
        free(result);
        errno = EBADMSG;
        return -1;
    }
    *out = result;
    return 0;
invalid:
    errno = EBADMSG;
    return -1;
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
int g1_validate_nbt(const uint8_t *data, size_t n, int32_t x, int32_t z) {
    mc_nbt_tag_t *root = NULL;
    size_t used = 0, len = 0;
    uint8_t *out = NULL;
    int rc = -1;
    if (!n || n > G1_MAX_RAW || mc_nbt_read_named_root(data, n, &root, &used) || used != n ||
        root->type != MC_NBT_TAG_COMPOUND || !unique(root, 0))
        goto done;
    const mc_nbt_tag_t *v = mc_nbt_compound_get(root, "DataVersion"), *a = mc_nbt_compound_get(root, "xPos"),
                       *b = mc_nbt_compound_get(root, "zPos"), *status = mc_nbt_compound_get(root, "Status");
    if (!v || v->type != MC_NBT_TAG_INT || v->payload.int_val != 4788 || !a || a->type != MC_NBT_TAG_INT ||
        a->payload.int_val != x || !b || b->type != MC_NBT_TAG_INT || b->payload.int_val != z || !status ||
        status->type != MC_NBT_TAG_STRING || strcmp(status->payload.string_val, "minecraft:full"))
        goto done;
    if (mc_nbt_write_named_root(root, &out, &len) || len != n || memcmp(out, data, n))
        goto done;
    rc = 0;
done:
    free(out);
    mc_nbt_free(root);
    if (rc)
        errno = EBADMSG;
    return rc;
}

int g1_sync(int fd) {
    int r;
    do {
        r = fsync(fd);
    } while (r < 0 && errno == EINTR);
    return r;
}
int g1_path(g1_store_t *s, const char *name, char path[4096]) {
    int n = snprintf(path, 4096, "/proc/self/fd/%d/%s", s->root, name);
    return n < 0 || n >= 4096 ? -1 : 0;
}
