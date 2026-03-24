#include "mc_chunk_store.h"

#include "mc_util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <zlib.h>

#define MC_CHUNK_STORE_MAGIC "MCC1"
#define MC_CHUNK_STORE_VERSION 1u
#define MC_CHUNK_STORE_HEADER_SIZE 36u

static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int chunk_store_path(char *buf, size_t cap, const char *world_path, int32_t cx, int32_t cz) {
    if (!buf || cap == 0 || !world_path || !*world_path) {
        errno = EINVAL;
        return -1;
    }
    int n = snprintf(buf, cap, "%s/chunks/c.%d.%d.mcc", world_path, cx, cz);
    if (n <= 0 || (size_t)n >= cap) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int deflate_buffer_zlib(const uint8_t *src, size_t src_len, uint8_t **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!src || !out || !out_len) {
        errno = EINVAL;
        return -1;
    }

    uLongf bound = compressBound((uLong)src_len);
    uint8_t *buf = (uint8_t *)malloc((size_t)bound ? (size_t)bound : 1u);
    if (!buf) return -1;

    int zrc = compress2(buf, &bound, src, (uLong)src_len, Z_DEFAULT_COMPRESSION);
    if (zrc != Z_OK) {
        free(buf);
        errno = EIO;
        return -1;
    }

    *out = buf;
    *out_len = (size_t)bound;
    return 0;
}

static int inflate_buffer_zlib(const uint8_t *src, size_t src_len, uint8_t *out, size_t out_len) {
    if (!src || !out) {
        errno = EINVAL;
        return -1;
    }
    uLongf got = (uLongf)out_len;
    int zrc = uncompress(out, &got, src, (uLong)src_len);
    if (zrc != Z_OK || got != (uLongf)out_len) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int mc_chunk_store_read(const char *world_path, int32_t cx, int32_t cz, mc_chunk_t *out) {
    if (!out) {
        errno = EINVAL;
        return -1;
    }
    if (!world_path || !*world_path) return 1;

    char path[1024];
    if (chunk_store_path(path, sizeof(path), world_path, cx, cz) != 0) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) {
        if (errno == ENOENT) return 1;
        log_error("chunk store read: open failed path=%s (errno=%d %s)", path, errno, strerror(errno));
        return -1;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        log_error("chunk store read: seek failed path=%s (errno=%d %s)", path, errno, strerror(errno));
        fclose(f);
        return -1;
    }
    long file_size = ftell(f);
    if (file_size < (long)MC_CHUNK_STORE_HEADER_SIZE) {
        log_error("chunk store read: file too small path=%s size=%ld", path, file_size);
        fclose(f);
        errno = EIO;
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        log_error("chunk store read: rewind failed path=%s (errno=%d %s)", path, errno, strerror(errno));
        fclose(f);
        return -1;
    }

    uint8_t header[MC_CHUNK_STORE_HEADER_SIZE];
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
        log_error("chunk store read: header short read path=%s", path);
        fclose(f);
        errno = EIO;
        return -1;
    }

    if (memcmp(header, MC_CHUNK_STORE_MAGIC, 4) != 0) {
        log_error("chunk store read: bad magic path=%s", path);
        fclose(f);
        errno = EINVAL;
        return -1;
    }

    uint32_t version = read_le32(header + 4);
    int32_t file_cx = (int32_t)read_le32(header + 8);
    int32_t file_cz = (int32_t)read_le32(header + 12);
    int32_t min_y = (int32_t)read_le32(header + 16);
    uint32_t height = read_le32(header + 20);
    uint32_t block_count = read_le32(header + 24);
    uint32_t raw_size = read_le32(header + 28);
    uint32_t want_crc = read_le32(header + 32);

    if (version != MC_CHUNK_STORE_VERSION || file_cx != cx || file_cz != cz || min_y != MC_WORLD_MIN_Y ||
        height != MC_WORLD_HEIGHT || block_count != MC_BLOCKS_PER_CHUNK ||
        raw_size != (uint32_t)(MC_BLOCKS_PER_CHUNK * sizeof(int32_t))) {
        log_error("chunk store read: invalid header path=%s version=%u cx=%d cz=%d min_y=%d height=%u blocks=%u raw=%u", path,
                  version, file_cx, file_cz, min_y, height, block_count, raw_size);
        fclose(f);
        errno = EINVAL;
        return -1;
    }

    size_t comp_len = (size_t)file_size - sizeof(header);
    uint8_t *comp = (uint8_t *)malloc(comp_len ? comp_len : 1u);
    if (!comp) {
        fclose(f);
        return -1;
    }
    if (fread(comp, 1, comp_len, f) != comp_len) {
        log_error("chunk store read: payload short read path=%s", path);
        free(comp);
        fclose(f);
        errno = EIO;
        return -1;
    }
    fclose(f);

    uint8_t *raw = (uint8_t *)malloc(raw_size ? raw_size : 1u);
    if (!raw) {
        free(comp);
        return -1;
    }
    if (inflate_buffer_zlib(comp, comp_len, raw, raw_size) != 0) {
        log_error("chunk store read: inflate failed path=%s (errno=%d %s)", path, errno, strerror(errno));
        free(raw);
        free(comp);
        return -1;
    }
    free(comp);

    uint32_t got_crc = crc32(0L, Z_NULL, 0);
    got_crc = crc32(got_crc, raw, raw_size);
    if (got_crc != want_crc) {
        log_error("chunk store read: crc mismatch path=%s want=%u got=%u", path, want_crc, got_crc);
        free(raw);
        errno = EIO;
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->cx = cx;
    out->cz = cz;
    for (size_t i = 0; i < MC_BLOCKS_PER_CHUNK; i++) {
        out->blocks[i] = (int32_t)read_le32(raw + (i * sizeof(uint32_t)));
    }
    out->loaded = true;
    out->dirty = false;
    out->evict_after_save = false;
    free(raw);
    return 0;
}

int mc_chunk_store_write(const char *world_path, const mc_chunk_t *chunk) {
    if (!world_path || !*world_path || !chunk) {
        errno = EINVAL;
        return -1;
    }

    char path[1024];
    char tmp_path[1100];
    if (chunk_store_path(path, sizeof(path), world_path, chunk->cx, chunk->cz) != 0) return -1;
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld", path, (long)getpid());
    if (n <= 0 || (size_t)n >= sizeof(tmp_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    size_t raw_size = MC_BLOCKS_PER_CHUNK * sizeof(uint32_t);
    uint8_t *raw = (uint8_t *)malloc(raw_size ? raw_size : 1u);
    if (!raw) return -1;
    for (size_t i = 0; i < MC_BLOCKS_PER_CHUNK; i++) {
        write_le32(raw + (i * sizeof(uint32_t)), (uint32_t)chunk->blocks[i]);
    }

    uint32_t crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, raw, raw_size);

    uint8_t *comp = NULL;
    size_t comp_len = 0;
    if (deflate_buffer_zlib(raw, raw_size, &comp, &comp_len) != 0) {
        log_error("chunk store write: compress failed path=%s (errno=%d %s)", path, errno, strerror(errno));
        free(raw);
        return -1;
    }
    free(raw);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        log_error("chunk store write: open failed path=%s (errno=%d %s)", tmp_path, errno, strerror(errno));
        free(comp);
        return -1;
    }

    uint8_t header[MC_CHUNK_STORE_HEADER_SIZE];
    memset(header, 0, sizeof(header));
    memcpy(header, MC_CHUNK_STORE_MAGIC, 4);
    write_le32(header + 4, MC_CHUNK_STORE_VERSION);
    write_le32(header + 8, (uint32_t)chunk->cx);
    write_le32(header + 12, (uint32_t)chunk->cz);
    write_le32(header + 16, (uint32_t)MC_WORLD_MIN_Y);
    write_le32(header + 20, (uint32_t)MC_WORLD_HEIGHT);
    write_le32(header + 24, (uint32_t)MC_BLOCKS_PER_CHUNK);
    write_le32(header + 28, (uint32_t)raw_size);
    write_le32(header + 32, crc);

    int rc = -1;
    if (fwrite(header, 1, sizeof(header), f) == sizeof(header) && fwrite(comp, 1, comp_len, f) == comp_len &&
        fflush(f) == 0 && fsync(fileno(f)) == 0) {
        rc = 0;
    }
    if (fclose(f) != 0 && rc == 0) rc = -1;
    free(comp);

    if (rc != 0) {
        log_error("chunk store write: failed path=%s (errno=%d %s)", tmp_path, errno, strerror(errno));
        unlink(tmp_path);
        return -1;
    }
    if (rename(tmp_path, path) != 0) {
        log_error("chunk store write: rename failed %s -> %s (errno=%d %s)", tmp_path, path, errno, strerror(errno));
        unlink(tmp_path);
        return -1;
    }
    return 0;
}
