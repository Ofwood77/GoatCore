#include "mc_chunk_store.h"

#include "mc_anvil.h"
#include "mc_atomic_file.h"
#include "mc_util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <zlib.h>

static int mkdir_p_local(const char *path, mode_t mode) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0;
        errno = ENOTDIR; return -1;
    }
    if (errno != ENOENT) return -1;
    return mc_persistence_mkdirs(path, mode);
}

static int chunks_dir_path(char *buf, size_t cap, const char *world_path) {
    int n;
    if (!buf || !world_path || !*world_path) {
        errno = EINVAL;
        return -1;
    }
    n = snprintf(buf, cap, "%s/chunks", world_path);
    if (n <= 0 || (size_t)n >= cap) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int chunk_file_path(char *buf, size_t cap, const char *world_path, int32_t cx, int32_t cz) {
    int n;

    if (!buf || !world_path || !*world_path) {
        errno = EINVAL;
        return -1;
    }
    n = snprintf(buf, cap, "%s/chunks/c_%d_%d.bin", world_path, cx, cz);
    if (n <= 0 || (size_t)n >= cap) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int deflate_buffer_zlib_local(const uint8_t *src, size_t src_len, uint8_t **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!src || !out || !out_len) return -1;
    if (src_len > (size_t)UINT_MAX) return -1;

    uLong bound = compressBound((uLong)src_len);
    if (bound == 0) return -1;

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

static int inflate_buffer_zlib_or_gzip_local(const uint8_t *src, size_t src_len, uint8_t **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!src || !out || !out_len) return -1;

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.next_in = (Bytef *)src;
    strm.avail_in = (uInt)src_len;

    if (inflateInit2(&strm, 15 + 32) != Z_OK) return -1;

    size_t cap = src_len ? (src_len * 4u) : 4096u;
    if (cap < 4096u) cap = 4096u;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) {
        inflateEnd(&strm);
        return -1;
    }

    int ret = Z_OK;
    while (ret != Z_STREAM_END) {
        if (strm.total_out >= cap) {
            size_t new_cap = cap * 2u;
            if (new_cap < cap) {
                free(buf);
                inflateEnd(&strm);
                return -1;
            }
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

int mc_chunk_store_read(const char *world_path, int32_t cx, int32_t cz, mc_chunk_t *out, mc_block_entity_store_t *be_store) {
    char path[1024];
    mc_block_entity_store_t dummy_store;
    mc_arena_t temp_arena;
    uint8_t *compressed = NULL;
    uint8_t *raw = NULL;
    size_t compressed_len = 0;
    size_t raw_len = 0;
    int rc = -1;

    if (!out) {
        errno = EINVAL;
        return -1;
    }
    if (!world_path || !*world_path) return 1;
    if (chunk_file_path(path, sizeof(path), world_path, cx, cz) != 0) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) {
        if (errno == ENOENT) return 1;
        return -1;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long size_l = ftell(f);
    if (size_l < 0) {
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }

    compressed_len = (size_t)size_l;
    compressed = (uint8_t *)malloc(compressed_len ? compressed_len : 1u);
    if (!compressed) {
        fclose(f);
        return -1;
    }
    if (compressed_len > 0 && fread(compressed, 1, compressed_len, f) != compressed_len) {
        free(compressed);
        fclose(f);
        return -1;
    }
    fclose(f);

    if (inflate_buffer_zlib_or_gzip_local(compressed, compressed_len, &raw, &raw_len) != 0) {
        free(compressed);
        return -1;
    }
    free(compressed);

    if (mc_chunk_init(out, cx, cz, 0u) != 0) {
        free(raw);
        return -1;
    }
    if (mc_arena_init(&temp_arena, 2u * 1024u * 1024u) != 0) {
        mc_chunk_destroy(out);
        free(raw);
        return -1;
    }
    if (!be_store) {
        mc_be_store_init(&dummy_store);
        be_store = &dummy_store;
    }

    rc = mc_anvil_decode_chunk_nbt(raw, raw_len, cx, cz, out, be_store, &temp_arena);

    if (be_store == &dummy_store) mc_be_store_destroy(&dummy_store);
    mc_arena_destroy(&temp_arena);
    free(raw);
    if (rc != 0) mc_chunk_destroy(out);
    return rc;
}

int mc_chunk_store_write(const char *world_path, const mc_chunk_t *chunk, const mc_block_entity_store_t *be_store) {
    char chunks_dir[1024];
    char chunk_path[1024];
    uint8_t *raw = NULL;
    uint8_t *compressed = NULL;
    size_t raw_len = 0;
    size_t compressed_len = 0;
    int rc = -1;
    mc_save_clock_t total = mc_save_profile_begin(), span;

    if (!world_path || !*world_path || !chunk) {
        errno = EINVAL;
        return -1;
    }
    if (chunks_dir_path(chunks_dir, sizeof(chunks_dir), world_path) != 0) return -1;
    if (mkdir_p_local(chunks_dir, 0755) != 0) return -1;
    if (chunk_file_path(chunk_path, sizeof(chunk_path), world_path, chunk->cx, chunk->cz) != 0) return -1;

    if (mc_anvil_encode_chunk_nbt(chunk, be_store, &raw, &raw_len) != 0) goto cleanup;
    span = mc_save_profile_begin();
    if (deflate_buffer_zlib_local(raw, raw_len, &compressed, &compressed_len) != 0) goto cleanup;
    mc_save_profile_end("chunk_compress", chunk->cx, chunk->cz, span, compressed_len, 0);

    mc_atomic_file_t file;
    if (mc_atomic_file_begin(&file, chunk_path) != 0) goto cleanup;
    span = mc_save_profile_begin();
    if (mc_file_write_all(file.temp_fd, compressed, compressed_len) != 0) {
        mc_atomic_file_abort(&file);
        goto cleanup;
    }
    mc_save_profile_end("chunk_temporary_write", chunk->cx, chunk->cz, span, compressed_len, 0);
    rc = mc_atomic_file_commit(&file);

cleanup:
    free(raw);
    free(compressed);
    mc_save_profile_end("chunk_total", chunk->cx, chunk->cz, total, compressed_len, rc);
    return rc;
}
