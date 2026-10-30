#include "mc_region_storage.h"
#include "mc_atomic_file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

struct mc_region_index {
    uint8_t header[MC_REGION_HEADER_BYTES];
    mc_region_entry_t entries[MC_REGION_SLOTS];
    mc_region_stats_t stats;
    mc_region_source_t source;
    uint64_t *used;
};
struct mc_region_file {
    int fd;
    mc_region_index_t *index;
};
struct mc_region_plan {
    uint64_t *used;
    uint32_t sectors, source_sectors;
    mc_region_source_t source;
    mc_region_entry_t entries[MC_REGION_SLOTS];
    mc_region_retirement_t retirements[MC_REGION_SLOTS];
    bool retired[MC_REGION_SLOTS], allocated;
    mc_region_reservation_t *reservations;
    size_t count, capacity;
};
static atomic_uint_fast64_t next_ticket = 1;
static size_t words(uint32_t n) {
    return ((size_t)n + 63) / 64;
}
static bool used(const uint64_t *map, uint32_t s) {
    return (map[s / 64] >> (s % 64)) & 1;
}
static void mark(uint64_t *map, uint32_t s, bool value) {
    uint64_t mask = UINT64_C(1) << (s % 64);
    if (value)
        map[s / 64] |= mask;
    else
        map[s / 64] &= ~mask;
}
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static int stat_fd(int fd, struct stat *st) {
    int rc;
    do {
        rc = fstat(fd, st);
    } while (rc < 0 && errno == EINTR);
    return rc;
}
static bool same(const mc_region_source_t *a, const struct stat *b) {
    /* nlink/ctime can change when a writer atomically replaces this inode. */
    return a->device == (uint64_t)b->st_dev && a->inode == (uint64_t)b->st_ino &&
           a->file_bytes == (uint64_t)b->st_size && a->mtime_seconds == b->st_mtim.tv_sec &&
           a->mtime_nanoseconds == b->st_mtim.tv_nsec;
}
int mc_region_index_load(int fd, mc_region_index_t **out) {
    if (!out) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    struct stat st;
    if (stat_fd(fd, &st) != 0)
        return -1;
    if (!S_ISREG(st.st_mode) || st.st_size < MC_REGION_HEADER_BYTES || st.st_size % MC_REGION_SECTOR_BYTES) {
        errno = EINVAL;
        return -1;
    }
    uint64_t n = (uint64_t)st.st_size / MC_REGION_SECTOR_BYTES;
    if (n > MC_REGION_MAX_SECTORS) {
        errno = EFBIG;
        return -1;
    }
    mc_region_index_t *v = calloc(1, sizeof(*v));
    if (!v)
        return -1;
    v->used = calloc(words((uint32_t)n), sizeof(uint64_t));
    if (!v->used) {
        free(v);
        return -1;
    }
    v->source = (mc_region_source_t){(uint64_t)st.st_dev, (uint64_t)st.st_ino, (uint64_t)st.st_size,
                                     st.st_mtim.tv_sec,   st.st_mtim.tv_nsec,  0};
    v->stats.file_bytes = (uint64_t)st.st_size;
    v->stats.sectors = (uint32_t)n;
    v->stats.occupied = 2;
    v->stats.metadata_bytes = sizeof(*v) + words((uint32_t)n) * sizeof(uint64_t);
    mark(v->used, 0, true);
    mark(v->used, 1, true);
    if (mc_file_pread_all(fd, v->header, sizeof(v->header), 0) != 0)
        goto fail;
    v->source.header_crc32 = (uint32_t)crc32(0, v->header, sizeof(v->header));
    for (unsigned i = 0; i < MC_REGION_SLOTS; i++) {
        mc_region_entry_t *e = &v->entries[i];
        uint32_t raw = be32(v->header + 4 * i);
        e->timestamp = be32(v->header + 4096 + 4 * i);
        if (!raw)
            continue;
        e->offset = raw >> 8;
        e->sectors = raw & 255;
        if (e->offset < 2 || !e->sectors || (uint64_t)e->offset + e->sectors > n) {
            errno = EINVAL;
            goto fail;
        }
        for (uint32_t s = e->offset; s < e->offset + e->sectors; s++) {
            if (used(v->used, s)) {
                errno = EINVAL;
                goto fail;
            }
            mark(v->used, s, true);
        }
        uint8_t record[5];
        if (mc_file_pread_all(fd, record, sizeof(record), (off_t)e->offset * 4096) != 0)
            goto fail;
        e->length = be32(record);
        e->compression = record[4];
        if (!e->length || (uint64_t)e->length + 4 > (uint64_t)e->sectors * 4096) {
            errno = EINVAL;
            goto fail;
        }
        e->external = (e->compression & 128) != 0;
        uint8_t codec = e->compression & 127;
        e->decodable = !e->external && codec >= 1 && codec <= 3;
        v->stats.chunks++;
        v->stats.occupied += e->sectors;
        v->stats.external += e->external;
        v->stats.unsupported += codec < 1 || codec > 3;
    }
    if (stat_fd(fd, &st) != 0)
        goto fail;
    if (!same(&v->source, &st)) {
        errno = ESTALE;
        goto fail;
    }
    v->stats.free_sectors = v->stats.sectors - v->stats.occupied;
    *out = v;
    return 0;
fail:;
    int saved = errno;
    mc_region_index_free(v);
    errno = saved;
    return -1;
}
void mc_region_index_free(mc_region_index_t *v) {
    if (v) {
        free(v->used);
        free(v);
    }
}
const mc_region_entry_t *mc_region_entry(const mc_region_index_t *v, unsigned slot) {
    return v && slot < MC_REGION_SLOTS ? &v->entries[slot] : NULL;
}
const mc_region_stats_t *mc_region_stats(const mc_region_index_t *v) {
    return v ? &v->stats : NULL;
}
const mc_region_source_t *mc_region_source(const mc_region_index_t *v) {
    return v ? &v->source : NULL;
}
const uint8_t *mc_region_header(const mc_region_index_t *v) {
    return v ? v->header : NULL;
}
bool mc_region_sector_used(const mc_region_index_t *v, uint32_t s) {
    return v && s < v->stats.sectors && used(v->used, s);
}
void mc_region_coordinates(int32_t cx, int32_t cz, int32_t *rx, int32_t *rz, unsigned *slot) {
    int32_t lx = cx % 32, lz = cz % 32;
    if (lx < 0)
        lx += 32;
    if (lz < 0)
        lz += 32;
    if (rx)
        *rx = (int32_t)(((int64_t)cx - lx) / 32);
    if (rz)
        *rz = (int32_t)(((int64_t)cz - lz) / 32);
    if (slot)
        *slot = (unsigned)(lx + 32 * lz);
}
int mc_region_open(const char *path, mc_region_file_t **out) {
    if (out)
        *out = NULL;
    if (!path || !out) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    int fd;
    do {
        fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return errno == ENOENT ? 1 : -1;
    mc_region_file_t *f = malloc(sizeof(*f));
    if (!f) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    f->fd = fd;
    if (mc_region_index_load(fd, &f->index) != 0) {
        int saved = errno;
        close(fd);
        free(f);
        errno = saved;
        return -1;
    }
    *out = f;
    return 0;
}
int mc_region_close(mc_region_file_t *f) {
    if (!f)
        return 0;
    int rc = close(f->fd), saved = errno; /* Linux close(EINTR) consumes fd. */
    mc_region_index_free(f->index);
    free(f);
    errno = saved;
    return rc;
}
const mc_region_index_t *mc_region_file_index(const mc_region_file_t *f) {
    return f ? f->index : NULL;
}
int mc_region_read_record(mc_region_file_t *f, unsigned slot, uint8_t **out, size_t *length) {
    if (out)
        *out = NULL;
    if (length)
        *length = 0;
    if (!f || !out || !length || slot >= MC_REGION_SLOTS) {
        errno = EINVAL;
        return -1;
    }
    struct stat st;
    if (stat_fd(f->fd, &st) != 0)
        return -1;
    if (!same(&f->index->source, &st)) {
        errno = ESTALE;
        return -1;
    }
    const mc_region_entry_t *e = &f->index->entries[slot];
    if (!e->offset)
        return 1;
    size_t n = (size_t)e->length + 4;
    uint8_t *buf = malloc(n);
    if (!buf)
        return -1;
    if (mc_file_pread_all(f->fd, buf, n, (off_t)e->offset * 4096) != 0)
        goto fail;
    if (stat_fd(f->fd, &st) != 0)
        goto fail;
    if (!same(&f->index->source, &st) || be32(buf) != e->length || buf[4] != e->compression) {
        errno = ESTALE;
        goto fail;
    }
    *out = buf;
    *length = n;
    return 0;
fail:;
    int saved = errno;
    free(buf);
    errno = saved;
    return -1;
}
int mc_region_plan_create(const mc_region_index_t *v, mc_region_plan_t **out) {
    if (!out) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    if (v && (v->stats.external || v->stats.unsupported)) {
        errno = EOPNOTSUPP;
        return -1;
    }
    mc_region_plan_t *p = calloc(1, sizeof(*p));
    if (!p)
        return -1;
    p->sectors = p->source_sectors = v ? v->stats.sectors : 2;
    p->used = calloc(words(p->sectors), sizeof(uint64_t));
    if (!p->used) {
        free(p);
        return -1;
    }
    if (v) {
        memcpy(p->used, v->used, words(p->sectors) * sizeof(uint64_t));
        memcpy(p->entries, v->entries, sizeof(p->entries));
        p->source = v->source;
    } else {
        mark(p->used, 0, true);
        mark(p->used, 1, true);
    }
    *out = p;
    return 0;
}
void mc_region_plan_free(mc_region_plan_t *p) {
    if (p) {
        free(p->used);
        free(p->reservations);
        free(p);
    }
}
int mc_region_plan_protect(mc_region_plan_t *p, uint32_t off, uint32_t n) {
    if (!p || off < 2 || !n || (uint64_t)off + n > p->source_sectors) {
        errno = EINVAL;
        return -1;
    }
    if (p->allocated) {
        errno = EBUSY;
        return -1;
    }
    for (uint32_t s = off; s < off + n; s++)
        mark(p->used, s, true);
    return 0;
}
int mc_region_plan_reserve_at(mc_region_plan_t *p, uint32_t off, uint32_t n, mc_region_reservation_t *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!p || !out || off < 2 || !n || n > 255) {
        errno = EINVAL;
        return -1;
    }
    if (off > MC_REGION_MAX_OFFSET || (uint64_t)off + n > MC_REGION_MAX_SECTORS) {
        errno = EFBIG;
        return -1;
    }
    for (uint32_t s = off; s < off + n && s < p->sectors; s++)
        if (used(p->used, s)) {
            errno = EEXIST;
            return -1;
        }
    size_t pos;
    for (pos = 0; pos < p->count && p->reservations[pos].ticket; pos++) {
    }
    if (pos == p->capacity) {
        size_t cap = p->capacity ? p->capacity * 2 : 16;
        if (cap < p->capacity || cap > SIZE_MAX / sizeof(*p->reservations)) {
            errno = ENOMEM;
            return -1;
        }
        void *r = realloc(p->reservations, cap * sizeof(*p->reservations));
        if (!r)
            return -1;
        p->reservations = r;
        p->capacity = cap;
    }
    uint32_t end = off + n;
    if (end > p->sectors) {
        size_t old_words = words(p->sectors), new_words = words(end);
        void *m = realloc(p->used, new_words * sizeof(uint64_t));
        if (!m)
            return -1;
        p->used = m;
        memset(p->used + old_words, 0, (new_words - old_words) * sizeof(uint64_t));
        p->sectors = end;
    }
    /* Never wrap/reuse tickets, even across independent plans. */
    uint_fast64_t ticket = atomic_load(&next_ticket);
    do {
        if (ticket == UINT64_MAX) {
            errno = EOVERFLOW;
            return -1;
        }
    } while (!atomic_compare_exchange_weak(&next_ticket, &ticket, ticket + 1));
    for (uint32_t s = off; s < end; s++)
        mark(p->used, s, true);
    *out = (mc_region_reservation_t){off, n, ticket};
    p->reservations[pos] = *out;
    if (pos == p->count)
        p->count++;
    p->allocated = true;
    return 0;
}
int mc_region_plan_reserve(mc_region_plan_t *p, uint32_t n, mc_region_reservation_t *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!p || !out || !n || n > 255) {
        errno = EINVAL;
        return -1;
    }
    uint32_t off = 2, run = 0;
    for (uint32_t s = 2; s < p->sectors; s++) {
        if (used(p->used, s)) {
            off = s + 1;
            run = 0;
        } else if (++run == n)
            return mc_region_plan_reserve_at(p, off, n, out);
    }
    return mc_region_plan_reserve_at(p, off, n, out);
}
int mc_region_plan_cancel(mc_region_plan_t *p, mc_region_reservation_t r) {
    if (!p || !r.ticket) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < p->count; i++) {
        mc_region_reservation_t *own = &p->reservations[i];
        if (own->ticket != r.ticket || own->offset != r.offset || own->sectors != r.sectors)
            continue;
        for (uint32_t s = r.offset; s < r.offset + r.sectors; s++)
            mark(p->used, s, false);
        own->ticket = 0;
        return 0;
    }
    errno = ENOENT;
    return -1;
}
int mc_region_plan_retire(mc_region_plan_t *p, unsigned slot, uint64_t epoch) {
    if (!p || slot >= MC_REGION_SLOTS || !p->entries[slot].offset) {
        errno = EINVAL;
        return -1;
    }
    if (p->retired[slot]) {
        errno = EALREADY;
        return -1;
    }
    const mc_region_entry_t *e = &p->entries[slot];
    p->retirements[slot] = (mc_region_retirement_t){slot, e->offset, e->sectors, epoch};
    p->retired[slot] = true;
    return 0;
}
const mc_region_retirement_t *mc_region_plan_retirement(const mc_region_plan_t *p, unsigned slot) {
    return p && slot < MC_REGION_SLOTS && p->retired[slot] ? &p->retirements[slot] : NULL;
}
const mc_region_source_t *mc_region_plan_source(const mc_region_plan_t *p) {
    return p ? &p->source : NULL;
}
uint32_t mc_region_plan_sectors(const mc_region_plan_t *p) {
    return p ? p->sectors : 0;
}
bool mc_region_plan_used(const mc_region_plan_t *p, uint32_t s) {
    return p && s < p->sectors && used(p->used, s);
}
