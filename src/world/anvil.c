#include "mc_anvil.h"

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
