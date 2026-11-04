#define _GNU_SOURCE
#include "g1_internal.h"
#include "mc_atomic_file.h"
#include "mc_anvil.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

static bool owned(g1_store_t *s) {
    if (!s || !pthread_equal(s->owner, pthread_self())) {
        errno = EPERM;
        return false;
    }
    return true;
}
static int bad(void) {
    errno = EBADMSG;
    return -1;
}
static int close_fd(int fd, int rc) {
    int error = errno;
    if (close(fd) && !rc) {
        rc = -1;
        error = errno;
    }
    errno = error;
    return rc;
}
static int random_bytes(void *data, size_t n) {
    uint8_t *p = data;
    while (n) {
        ssize_t r = getrandom(p, n, 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}
static int open_directory(const char *path) {
    char *copy = strdup(path), *save = NULL;
    if (!copy)
        return -1;
    int fd = open(*path == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    for (char *part = strtok_r(copy, "/", &save); part && fd >= 0; part = strtok_r(NULL, "/", &save)) {
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        int e = errno;
        close(fd);
        fd = next;
        errno = e;
    }
    free(copy);
    return fd;
}
static int scan_source(int fd, const struct stat *parent, const struct stat report[2], const bool regular[2],
                       unsigned depth, size_t *entries) {
    if (depth > 32) {
        errno = ELOOP;
        return -1;
    }
    struct stat here;
    if (fstat(fd, &here))
        return -1;
    if (here.st_dev == parent->st_dev && here.st_ino == parent->st_ino) {
        errno = EINVAL;
        return -1;
    }
    DIR *dir = fdopendir(openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!dir)
        return -1;
    int rc = 0;
    for (;;) {
        errno = 0;
        struct dirent *de = readdir(dir);
        if (!de) {
            if (errno)
                rc = -1;
            break;
        }
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (++*entries > 100000) {
            errno = EFBIG;
            rc = -1;
            break;
        }
        struct stat st;
        if (fstatat(fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
            rc = -1;
            break;
        }
        for (unsigned i = 0; i < 2; i++)
            if (regular[i] && st.st_dev == report[i].st_dev && st.st_ino == report[i].st_ino)
                rc = -4;
        if (rc)
            break;
        if (S_ISDIR(st.st_mode)) {
            int child = openat(fd, de->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (child < 0) {
                rc = -1;
                break;
            }
            rc = scan_source(child, parent, report, regular, depth + 1, entries);
            rc = close_fd(child, rc);
            if (rc)
                break;
        }
    }
    int error = errno;
    if (closedir(dir) && !rc) {
        rc = -1;
        error = errno;
    }
    errno = error;
    return rc;
}
int g1_disjoint(const char *source, const char *destination) {
    int src = open_directory(source), parent = -1;
    char *copy = NULL;
    int rc = -1;
    struct stat parent_stat;
    if (src < 0)
        return -1;
    copy = strdup(destination);
    if (!copy)
        goto done;
    char *name = copy, *slash = strrchr(copy, '/');
    if (slash) {
        *slash = 0;
        name = slash + 1;
        parent = open_directory(*copy ? copy : "/");
    } else
        parent = open_directory(".");
    if (parent < 0 || !*name || !strcmp(name, ".") || !strcmp(name, "..")) {
        errno = EINVAL;
        goto done;
    }
    struct stat existing;
    if (!fstatat(parent, name, &existing, AT_SYMLINK_NOFOLLOW) || errno != ENOENT) {
        errno = EEXIST;
        goto done;
    }
    if (fstat(parent, &parent_stat))
        goto done;
    struct stat reports[2];
    bool regular[2] = {false, false};
    for (int i = 1; i <= 2; i++)
        regular[i - 1] = !fstat(i, &reports[i - 1]) && S_ISREG(reports[i - 1].st_mode);
    size_t entries = 0;
    rc = scan_source(src, &parent_stat, reports, regular, 0, &entries);
done:;
    int error = errno;
    if (parent >= 0)
        rc = close_fd(parent, rc);
    rc = close_fd(src, rc);
    free(copy);
    if (rc)
        errno = error;
    return rc;
}
static void region_name(char name[128], uint32_t dim, int32_t rx, int32_t rz, const char *suffix) {
    snprintf(name, 128, "d.%u.r.%d.%d.%s", dim, rx, rz, suffix);
}
static void data_name(char name[128], uint32_t dim, int32_t rx, int32_t rz, uint64_t id) {
    char suffix[32];
    snprintf(suffix, sizeof(suffix), "data.%016" PRIx64, id);
    region_name(name, dim, rx, rz, suffix);
}
static void sign_index(uint8_t *b) {
    memset(b + 96, 0, 32);
    if (g1_sha(b, G1_INDEX_BYTES, b + 96))
        abort();
}
static bool valid_index(uint8_t *b, uint32_t dim, int32_t rx, int32_t rz, const uint8_t id[16]) {
    uint8_t expected[32], actual[32];
    memcpy(expected, b + 96, 32);
    memset(b + 96, 0, 32);
    int r = g1_sha(b, G1_INDEX_BYTES, actual);
    memcpy(b + 96, expected, 32);
    return !r && !memcmp(actual, expected, 32) && !memcmp(b, "GWIDX001", 8) && g1_u32(b + 8) == 1 &&
           g1_u32(b + 12) == 128 && g1_u32(b + 16) == 4788 && g1_u32(b + 20) == dim &&
           (int32_t)g1_u32(b + 24) == rx && (int32_t)g1_u32(b + 28) == rz && g1_u32(b + 56) == 1024 &&
           g1_u32(b + 60) == 32 && !memcmp(b + 64, id, 16) && !memcmp(b + 80, (uint8_t[16]){0}, 16) &&
           g1_u64(b + 32) > 0 && g1_u64(b + 40) > 0;
}
static void data_header(uint8_t b[128], g1_store_t *s, uint32_t dim, int32_t rx, int32_t rz, uint64_t id) {
    memset(b, 0, 128);
    memcpy(b, "GWLOG001", 8);
    g1_p32(b + 8, 1);
    g1_p32(b + 12, 128);
    g1_p32(b + 16, 4788);
    g1_p32(b + 20, dim);
    g1_p32(b + 24, (uint32_t)rx);
    g1_p32(b + 28, (uint32_t)rz);
    g1_p64(b + 40, id);
    memcpy(b + 64, s->id, 16);
    if (g1_sha(b, 96, b + 96))
        abort();
}
static bool valid_data(const uint8_t b[128], g1_store_t *s, uint32_t dim, int32_t rx, int32_t rz,
                       uint64_t id) {
    uint8_t expected[128];
    data_header(expected, s, dim, rx, rz, id);
    return !memcmp(b, expected, 128);
}
static void evict_regions(g1_store_t *s) {
    for (unsigned i = 0; i < G1_REGIONS; i++) {
        struct g1_region *r = &s->regions[i];
        if (r->present) {
            if (r->anvil)
                mc_region_close(r->anvil);
            else
                close(r->fd);
            free(r->index);
        }
        memset(r, 0, sizeof(*r));
        r->fd = -1;
    }
}
struct interval {
    uint64_t a, b;
};
static int by_offset(const void *a, const void *b) {
    const struct interval *x = a, *y = b;
    return x->a < y->a ? -1 : x->a > y->a;
}
static int load_region(g1_store_t *s, uint32_t dim, int32_t rx, int32_t rz, struct g1_region **out) {
    struct g1_region *victim = &s->regions[0];
    for (unsigned i = 0; i < G1_REGIONS; i++) {
        struct g1_region *r = &s->regions[i];
        if (r->present && r->dim == dim && r->rx == rx && r->rz == rz) {
            r->used = ++s->clock;
            *out = r;
            return 0;
        }
        if (!r->present || r->used < victim->used)
            victim = r;
    }
    char name[128];
    region_name(name, dim, rx, rz, "idx");
    int fd = openat(s->root, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno != ENOENT)
            return -1;
        region_name(name, dim, rx, rz, "owner");
        struct stat owner;
        if (!fstatat(s->root, name, &owner, AT_SYMLINK_NOFOLLOW)) {
            errno = EUCLEAN;
            return -1;
        }
        return errno == ENOENT ? 1 : -1;
    }
    uint8_t *b = malloc(G1_INDEX_BYTES);
    struct stat st;
    int r = b ? 0 : -1;
    if (!r && (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size != G1_INDEX_BYTES ||
               mc_file_pread_all(fd, b, G1_INDEX_BYTES, 0) || !valid_index(b, dim, rx, rz, s->id)))
        r = bad();
    r = close_fd(fd, r);
    if (r) {
        free(b);
        return r;
    }
    data_name(name, dim, rx, rz, g1_u64(b + 40));
    fd = openat(s->root, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    uint8_t h[128];
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size < 128 ||
        g1_u64(b + 48) > (uint64_t)st.st_size || g1_u64(b + 48) < 128 || mc_file_pread_all(fd, h, 128, 0) ||
        !valid_data(h, s, dim, rx, rz, g1_u64(b + 40))) {
        if (fd >= 0)
            close(fd);
        free(b);
        return bad();
    }
    struct interval spans[1024];
    size_t spans_n = 0;
    for (unsigned slot = 0; slot < 1024; slot++) {
        uint8_t *e = b + 128 + slot * 32;
        uint64_t offset = g1_u64(e);
        uint32_t size = g1_u32(e + 8), raw = g1_u32(e + 12);
        if (!offset) {
            if (memcmp(e, (uint8_t[32]){0}, 32)) {
                r = bad();
                break;
            }
            continue;
        }
        if (offset < 4096 || size < 96 || size > 2u * G1_MAX_RAW + 96 || raw < 1 || raw > G1_MAX_RAW ||
            !g1_u64(e + 16) || offset > g1_u64(b + 48) || size > g1_u64(b + 48) - offset || e[27] ||
            g1_u32(e + 28)) {
            r = bad();
            break;
        }
        spans[spans_n++] = (struct interval){offset, offset + size};
    }
    qsort(spans, spans_n, sizeof(*spans), by_offset);
    for (size_t i = 1; i < spans_n; i++)
        if (spans[i].a < spans[i - 1].b)
            r = bad();
    if (r) {
        close(fd);
        free(b);
        return r;
    }
    if (victim->present) {
        close(victim->fd);
        free(victim->index);
    }
    *victim = (struct g1_region){
        .present = true, .dim = dim, .rx = rx, .rz = rz, .fd = fd, .used = ++s->clock, .index = b};
    *out = victim;
    return 0;
}
static int publish(g1_store_t *s, uint32_t dim, int32_t rx, int32_t rz, uint8_t *index) {
    char name[128], path[4096];
    region_name(name, dim, rx, rz, "idx");
    if (g1_path(s, name, path))
        return -1;
    sign_index(index);
    mc_atomic_file_t file;
    if (mc_atomic_file_begin(&file, path))
        return -1;
    G1_HOOK("index-write");
    if (mc_file_write_all(file.temp_fd, index, G1_INDEX_BYTES)) {
        mc_atomic_file_abort(&file);
        return -1;
    }
    G1_HOOK("index-ready");
    int r = mc_atomic_file_commit(&file);
    G1_HOOK("index-published");
    return r;
}
static int new_data(g1_store_t *s, uint32_t dim, int32_t rx, int32_t rz, uint64_t *id) {
    char name[128];
    if (random_bytes(id, sizeof(*id)) || !*id)
        return -1;
    data_name(name, dim, rx, rz, *id);
    int fd = openat(s->root, name, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
    uint8_t h[128];
    data_header(h, s, dim, rx, rz, *id);
    if (mc_file_write_all(fd, h, 128)) {
        close(fd);
        return -1;
    }
    return fd;
}
static int create_region(g1_store_t *s, uint32_t dim, int32_t rx, int32_t rz) {
    /* A missing index beside ANY old data is ambiguous: never invent a new root. */
    DIR *dir = fdopendir(openat(s->root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!dir)
        return -1;
    char prefix[128];
    region_name(prefix, dim, rx, rz, "data.");
    struct dirent *de;
    bool orphan = false;
    while ((de = readdir(dir)))
        if (!strncmp(de->d_name, prefix, strlen(prefix)))
            orphan = true;
    if (closedir(dir))
        return -1;
    if (orphan) {
        errno = EUCLEAN;
        return -1;
    }
    char owner_name[128];
    region_name(owner_name, dim, rx, rz, "owner");
    int owner_fd = openat(s->root, owner_name, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (owner_fd < 0)
        return -1;
    int owner_rc = g1_sync(owner_fd);
    owner_rc = close_fd(owner_fd, owner_rc);
    if (!owner_rc)
        owner_rc = g1_sync(s->root);
    if (owner_rc)
        return -1;
    uint64_t id;
    int fd = new_data(s, dim, rx, rz, &id);
    if (fd < 0)
        return -1;
    int rc = g1_sync(fd);
    rc = close_fd(fd, rc);
    if (!rc)
        rc = g1_sync(s->root);
    if (rc)
        return rc;
    uint8_t *b = calloc(1, G1_INDEX_BYTES);
    if (!b)
        return -1;
    memcpy(b, "GWIDX001", 8);
    g1_p32(b + 8, 1);
    g1_p32(b + 12, 128);
    g1_p32(b + 16, 4788);
    g1_p32(b + 20, dim);
    g1_p32(b + 24, (uint32_t)rx);
    g1_p32(b + 28, (uint32_t)rz);
    g1_p64(b + 32, 1);
    g1_p64(b + 40, id);
    g1_p64(b + 48, 128);
    g1_p32(b + 56, 1024);
    g1_p32(b + 60, 32);
    memcpy(b + 64, s->id, 16);
    rc = publish(s, dim, rx, rz, b);
    free(b);
    return rc;
}
static int native_read(g1_store_t *s, uint32_t dim, int32_t x, int32_t z, uint8_t **out, size_t *n,
                       uint64_t *gen) {
    int32_t rx, rz;
    unsigned slot;
    mc_region_coordinates(x, z, &rx, &rz, &slot);
    struct g1_region *region = NULL;
    int rc = load_region(s, dim, rx, rz, &region);
    if (rc)
        return rc;
    uint8_t *e = region->index + 128 + slot * 32;
    if (!g1_u64(e))
        return 1;
    uint8_t h[96];
    if (mc_file_pread_all(region->fd, h, 96, (off_t)g1_u64(e)))
        return -1;
    if (memcmp(h, "GWREC001", 8) || g1_u32(h + 8) != 1 || g1_u32(h + 12) != 96 ||
        (int32_t)g1_u32(h + 16) != x || (int32_t)g1_u32(h + 20) != z || g1_u32(h + 24) != dim ||
        g1_u32(h + 28) != g1_u32(e + 12) || !g1_u32(h + 32) || g1_u32(h + 32) > 2u * G1_MAX_RAW ||
        g1_u32(h + 32) + 96u != g1_u32(e + 8) || memcmp(h + 36, e + 24, 3) || h[39] ||
        g1_u64(h + 40) != g1_u64(e + 16) || g1_u32(h + 80) != (uint32_t)crc32(0, h, 80) ||
        memcmp(h + 84, (uint8_t[12]){0}, 12))
        return bad();
    size_t bytes = g1_u32(h + 32);
    uint8_t *encoded = malloc(bytes);
    if (!encoded)
        return -1;
    g1_profile_t p = {h[36], h[37], (int8_t)h[38]};
    rc = mc_file_pread_all(region->fd, encoded, bytes, (off_t)(g1_u64(e) + 96));
    if (!rc)
        rc = g1_decode(encoded, bytes, g1_u32(h + 28), p, out);
    free(encoded);
    if (rc)
        return rc;
    uint8_t digest[32];
    if (g1_sha(*out, g1_u32(h + 28), digest) || memcmp(digest, h + 48, 32)) {
        free(*out);
        *out = NULL;
        return bad();
    }
    *n = g1_u32(h + 28);
    *gen = g1_u64(h + 40);
    return 0;
}
static int append_record(int fd, uint64_t *end, uint32_t dim, const g1_update_t *u, g1_profile_t p,
                         uint8_t entry[32]) {
    uint8_t *encoded = NULL, h[96] = {0};
    size_t n = 0;
    if (g1_encode(u->raw, u->length, p, &encoded, &n))
        return -1;
    memcpy(h, "GWREC001", 8);
    g1_p32(h + 8, 1);
    g1_p32(h + 12, 96);
    g1_p32(h + 16, (uint32_t)u->x);
    g1_p32(h + 20, (uint32_t)u->z);
    g1_p32(h + 24, dim);
    g1_p32(h + 28, (uint32_t)u->length);
    g1_p32(h + 32, (uint32_t)n);
    h[36] = p.codec;
    h[37] = p.framed;
    h[38] = (uint8_t)p.level;
    g1_p64(h + 40, u->generation);
    int rc = g1_sha(u->raw, u->length, h + 48);
    g1_p32(h + 80, (uint32_t)crc32(0, h, 80));
    G1_HOOK("record-write");
    if (!rc)
        rc = mc_file_pwrite_all(fd, h, 96, (off_t)*end);
    if (!rc)
        rc = mc_file_pwrite_all(fd, encoded, n, (off_t)(*end + 96));
    free(encoded);
    if (rc)
        return rc;
    memset(entry, 0, 32);
    g1_p64(entry, *end);
    g1_p32(entry + 8, (uint32_t)n + 96);
    g1_p32(entry + 12, (uint32_t)u->length);
    g1_p64(entry + 16, u->generation);
    entry[24] = p.codec;
    entry[25] = p.framed;
    entry[26] = (uint8_t)p.level;
    *end += 96 + n;
    return 0;
}
static int native_write(g1_store_t *s, uint32_t dim, const g1_update_t *u, size_t count, g1_profile_t p) {
    int32_t rx, rz;
    unsigned slot;
    mc_region_coordinates(u[0].x, u[0].z, &rx, &rz, &slot);
    struct g1_region *region = NULL;
    int rc = load_region(s, dim, rx, rz, &region);
    if (rc == 1) {
        if (create_region(s, dim, rx, rz))
            return -1;
        rc = load_region(s, dim, rx, rz, &region);
    }
    if (rc)
        return rc;
    uint8_t *next = malloc(G1_INDEX_BYTES);
    if (!next)
        return -1;
    memcpy(next, region->index, G1_INDEX_BYTES);
    if (g1_u64(next + 32) == UINT64_MAX) {
        free(next);
        errno = EOVERFLOW;
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        mc_region_coordinates(u[i].x, u[i].z, NULL, NULL, &slot);
        if (u[i].generation <= g1_u64(next + 128 + slot * 32 + 16)) {
            free(next);
            errno = ESTALE;
            return -1;
        }
    }
    char name[128];
    data_name(name, dim, rx, rz, g1_u64(next + 40));
    int fd = openat(s->root, name, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (fd < 0) {
        free(next);
        return -1;
    }
    if (fstat(fd, &st) || st.st_size < 128 || (uint64_t)st.st_size > (UINT64_C(1) << 40)) {
        close(fd);
        free(next);
        return bad();
    }
    uint64_t end = ((uint64_t)st.st_size + 4095) & ~UINT64_C(4095);
    G1_HOOK("before-append");
    for (size_t i = 0; i < count; i++) {
        mc_region_coordinates(u[i].x, u[i].z, NULL, NULL, &slot);
        if (append_record(fd, &end, dim, &u[i], p, next + 128 + slot * 32)) {
            rc = -1;
            break;
        }
    }
    G1_HOOK("before-data-sync");
    if (!rc)
        rc = g1_sync(fd);
    G1_HOOK("after-data-sync");
    rc = close_fd(fd, rc);
    if (!rc) {
        g1_p64(next + 32, g1_u64(next + 32) + 1);
        g1_p64(next + 48, end);
        rc = publish(s, dim, rx, rz, next);
    }
    free(next);
    evict_regions(s);
    if (!rc)
        G1_HOOK("durable");
    return rc;
}
static int anvil_path(g1_store_t *s, uint32_t dim, int32_t rx, int32_t rz, char path[4096]) {
    char name[128];
    snprintf(name, sizeof(name), "anvil/d.%u/r.%d.%d.mca", dim, rx, rz);
    return g1_path(s, name, path);
}
static int anvil_read(g1_store_t *s, uint32_t dim, int32_t x, int32_t z, uint8_t **out, size_t *n,
                      uint64_t *gen) {
    int32_t rx, rz;
    unsigned slot;
    mc_region_coordinates(x, z, &rx, &rz, &slot);
    *gen = 0;
    struct g1_region *victim = &s->regions[0];
    for (unsigned i = 0; i < G1_REGIONS; i++) {
        struct g1_region *r = &s->regions[i];
        if (r->present && r->dim == dim && r->rx == rx && r->rz == rz) {
            r->used = ++s->clock;
            return mc_anvil_read_chunk_indexed(r->anvil, slot, out, n);
        }
        if (!r->present || r->used < victim->used)
            victim = r;
    }
    char path[4096];
    mc_region_file_t *file = NULL;
    if (anvil_path(s, dim, rx, rz, path))
        return -1;
    int rc = mc_region_open(path, &file);
    if (rc)
        return rc;
    if (victim->present) {
        mc_region_close(victim->anvil);
        free(victim->index);
    }
    *victim = (struct g1_region){
        .present = true, .dim = dim, .rx = rx, .rz = rz, .fd = -1, .used = ++s->clock, .anvil = file};
    return mc_anvil_read_chunk_indexed(file, slot, out, n);
}
static int anvil_write(g1_store_t *s, uint32_t dim, const g1_update_t *u, size_t n, g1_profile_t p) {
    if (p.codec != 1 || p.level != 6 || p.framed) {
        errno = EOPNOTSUPP;
        return -1;
    }
    int32_t rx, rz;
    unsigned slot;
    mc_region_coordinates(u[0].x, u[0].z, &rx, &rz, &slot);
    char path[4096];
    if (anvil_path(s, dim, rx, rz, path))
        return -1;
    mc_anvil_chunk_write_t batch[64];
    uint8_t *old[64] = {0};
    int rc = 0;
    for (size_t i = 0; i < n; i++) {
        size_t len = 0;
        uint64_t gen;
        mc_region_coordinates(u[i].x, u[i].z, NULL, NULL, &slot);
        rc = anvil_read(s, dim, u[i].x, u[i].z, &old[i], &len, &gen);
        if (rc < 0)
            break;
        batch[i] = (mc_anvil_chunk_write_t){slot, u[i].raw, u[i].length, old[i], len};
        rc = 0;
    }
    if (!rc)
        rc = mc_anvil_write_batch_expected(path, batch, n);
    for (size_t i = 0; i < n; i++)
        free(old[i]);
    return rc;
}
static void invalidate(g1_store_t *s) {
    for (unsigned i = 0; i < G1_CACHE_ENTRIES; i++) {
        free(s->cache[i].data);
        memset(&s->cache[i], 0, sizeof(s->cache[i]));
    }
    s->cache_bytes = 0;
}
int g1_read(g1_store_t *s, uint32_t dim, int32_t x, int32_t z, uint8_t **out, size_t *n, uint64_t *gen) {
    *out = NULL;
    *n = 0;
    *gen = 0;
    if (!owned(s))
        return -1;
    if (dim > 2) {
        errno = EOPNOTSUPP;
        return -1;
    }
    for (unsigned i = 0; i < G1_CACHE_ENTRIES; i++) {
        struct g1_cache *c = &s->cache[i];
        if (c->data && c->dim == dim && c->x == x && c->z == z) {
            *out = malloc(c->length);
            if (!*out)
                return -1;
            memcpy(*out, c->data, c->length);
            *n = c->length;
            *gen = c->gen;
            c->used = ++s->clock;
            s->hits++;
            return 0;
        }
    }
    s->misses++;
    s->reads++;
    int rc = s->backend == G1_NATIVE   ? native_read(s, dim, x, z, out, n, gen)
             : s->backend == G1_SQLITE ? g1_sql_read(s, dim, x, z, out, n, gen)
                                       : anvil_read(s, dim, x, z, out, n, gen);
    if (rc < 0) {
        s->poisoned = true;
        return rc;
    }
    if (rc || !*n || *n > s->cache_limit)
        return rc;
    unsigned at = 0;
    while (s->cache_bytes + *n > s->cache_limit) {
        at = 0;
        for (unsigned i = 0; i < G1_CACHE_ENTRIES; i++)
            if (s->cache[i].data && (!s->cache[at].data || s->cache[i].used < s->cache[at].used))
                at = i;
        s->cache_bytes -= s->cache[at].length;
        free(s->cache[at].data);
        memset(&s->cache[at], 0, sizeof(s->cache[at]));
    }
    at = 0;
    for (unsigned i = 0; i < G1_CACHE_ENTRIES; i++)
        if (!s->cache[i].data || s->cache[i].used < s->cache[at].used)
            at = i;
    struct g1_cache *c = &s->cache[at];
    s->cache_bytes -= c->length;
    free(c->data);
    memset(c, 0, sizeof(*c));
    c->data = malloc(*n);
    if (c->data) {
        memcpy(c->data, *out, *n);
        c->length = *n;
        c->dim = dim;
        c->x = x;
        c->z = z;
        c->gen = *gen;
        c->used = ++s->clock;
        s->cache_bytes += *n;
    }
    return 0;
}
int g1_write(g1_store_t *s, uint32_t dim, const g1_update_t *u, size_t n, g1_profile_t p) {
    if (!owned(s))
        return -1;
    if (!s->writable || s->poisoned || dim > 2 || !n || n > 64) {
        errno = EPERM;
        return -1;
    }
    size_t bytes = 0;
    int32_t rx, rz;
    mc_region_coordinates(u[0].x, u[0].z, &rx, &rz, NULL);
    G1_HOOK("prepare");
    for (size_t i = 0; i < n; i++) {
        int32_t a, b;
        mc_region_coordinates(u[i].x, u[i].z, &a, &b, NULL);
        if (a != rx || b != rz || !u[i].raw || !u[i].length || u[i].length > G1_MAX_RAW || !u[i].generation ||
            u[i].generation > INT64_MAX || bytes > G1_MAX_BATCH_BYTES - u[i].length) {
            errno = EINVAL;
            return -1;
        }
        bytes += u[i].length;
        if (g1_validate_nbt(u[i].raw, u[i].length, u[i].x, u[i].z))
            return -1;
        for (size_t j = 0; j < i; j++)
            if (u[i].x == u[j].x && u[i].z == u[j].z) {
                errno = EINVAL;
                return -1;
            }
        uint8_t *old = NULL, hash[32] = {0};
        size_t old_n = 0;
        uint64_t gen = 0;
        int rc = g1_read(s, dim, u[i].x, u[i].z, &old, &old_n, &gen);
        if (rc < 0)
            return rc;
        if (!rc && g1_sha(old, old_n, hash)) {
            free(old);
            return -1;
        }
        free(old);
        if (memcmp(hash, u[i].expected, 32)) {
            errno = ESTALE;
            return -1;
        }
    }
    int rc = s->backend == G1_NATIVE   ? native_write(s, dim, u, n, p)
             : s->backend == G1_SQLITE ? g1_sql_write(s, dim, u, n, p)
                                       : anvil_write(s, dim, u, n, p);
    invalidate(s);
    if (s->backend == G1_ANVIL)
        evict_regions(s);
    if (rc)
        s->poisoned = true;
    else
        s->writes += n;
    return rc;
}
int g1_open(const char *path, g1_backend_t backend, bool create, bool writable, size_t cache_limit,
            g1_store_t **out) {
    *out = NULL;
    if (backend > G1_ANVIL || cache_limit > 64u * 1024u * 1024u) {
        errno = EINVAL;
        return -1;
    }
    g1_store_t *s = calloc(1, sizeof(*s));
    if (!s)
        return -1;
    s->owner = pthread_self();
    s->root = s->lock = -1;
    s->backend = backend;
    s->writable = writable;
    s->cache_limit = cache_limit;
    int parent = -1;
    char *copy = NULL;
    if (create) {
        if (!writable) {
            errno = EINVAL;
            goto fail;
        }
        copy = strdup(path);
        if (!copy)
            goto fail;
        char *slash = strrchr(copy, '/');
        char *name = copy;
        if (slash) {
            *slash = 0;
            name = slash + 1;
            parent = open_directory(*copy ? copy : "/");
        } else
            parent = open_directory(".");
        if (parent < 0 || !*name || !strcmp(name, ".") || !strcmp(name, "..") || mkdirat(parent, name, 0700))
            goto fail;
        s->root = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    } else
        s->root = open_directory(path);
    if (s->root < 0 || !realpath(path, s->path))
        goto fail;
    uint8_t marker[64] = {0};
    int fd = openat(s->root, "GOATWORLD-G1",
                    (create ? O_CREAT | O_EXCL | O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        goto fail;
    int rc = 0;
    if (create) {
        memcpy(marker, "G1WORLD1", 8);
        g1_p32(marker + 8, 1);
        g1_p32(marker + 12, (uint32_t)backend);
        if (random_bytes(marker + 16, 16) || g1_sha(marker, 32, marker + 32) ||
            mc_file_write_all(fd, marker, 64) || g1_sync(fd))
            rc = -1;
    } else {
        struct stat st;
        uint8_t digest[32];
        if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size != 64 ||
            mc_file_pread_all(fd, marker, 64, 0) || memcmp(marker, "G1WORLD1", 8) ||
            g1_u32(marker + 8) != 1 || g1_u32(marker + 12) != (uint32_t)backend ||
            g1_sha(marker, 32, digest) || memcmp(marker + 32, digest, 32))
            rc = bad();
    }
    rc = close_fd(fd, rc);
    if (rc)
        goto fail;
    memcpy(s->id, marker + 16, 16);
    s->lock = openat(s->root, "OWNER.lock",
                     (create ? O_CREAT | O_EXCL | O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (s->lock < 0)
        goto fail;
    struct stat lock_stat;
    if (fstat(s->lock, &lock_stat) || !S_ISREG(lock_stat.st_mode) || lock_stat.st_nlink != 1 ||
        flock(s->lock, (writable ? LOCK_EX : LOCK_SH) | LOCK_NB))
        goto fail;
    if (create) {
        if (g1_sync(s->lock) || g1_sync(s->root) || g1_sync(parent))
            goto fail;
    }
    if (backend == G1_ANVIL && !create) {
        int dir = openat(s->root, "anvil", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (dir < 0)
            goto fail;
        int rc = 0;
        for (unsigned d = 0; d < 3; d++) {
            char name[32];
            snprintf(name, sizeof(name), "d.%u", d);
            int fd = openat(dir, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (fd < 0) {
                rc = -1;
                break;
            }
            if (close(fd)) {
                rc = -1;
                break;
            }
        }
        rc = close_fd(dir, rc);
        if (rc)
            goto fail;
    }
    if (backend == G1_SQLITE && g1_sql_open(s, create))
        goto fail;
    if (backend == G1_ANVIL && create) {
        if (mkdirat(s->root, "anvil", 0700))
            goto fail;
        int dir = openat(s->root, "anvil", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (dir < 0)
            goto fail;
        int error = 0;
        for (unsigned dim = 0; dim < 3 && !error; dim++) {
            char name[32];
            snprintf(name, sizeof(name), "d.%u", dim);
            if (mkdirat(dir, name, 0700)) {
                error = -1;
                break;
            }
            int child = openat(dir, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (child < 0) {
                error = -1;
                break;
            }
            error = g1_sync(child);
            error = close_fd(child, error);
        }
        if (!error)
            error = g1_sync(dir);
        error = close_fd(dir, error);
        if (!error)
            error = g1_sync(s->root);
        if (error)
            goto fail;
    }
    if (parent >= 0)
        close(parent);
    free(copy);
    *out = s;
    return 0;
fail:;
    int saved = errno;
    if (parent >= 0)
        close(parent);
    free(copy);
    if (s->sql)
        g1_sql_close(s);
    if (s->lock >= 0)
        close(s->lock);
    if (s->root >= 0)
        close(s->root);
    free(s);
    errno = saved;
    return -1;
}
int g1_close(g1_store_t *s) {
    if (!owned(s))
        return -1;
    if (!s)
        return 0;
    int rc = 0;
    if (s->sql) {
        if (s->writable)
            rc = g1_sql_finish(s, false);
        if (g1_sql_close(s) != 0)
            rc = -1;
    }
    invalidate(s);
    evict_regions(s);
    if (s->lock >= 0)
        rc = close_fd(s->lock, rc);
    if (s->root >= 0)
        rc = close_fd(s->root, rc);
    free(s);
    return rc;
}
void g1_cache_stats(g1_store_t *s, g1_cache_stats_t *out) {
    if (!owned(s)) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = (g1_cache_stats_t){s->hits, s->misses, s->cache_bytes, s->cache_limit, s->reads, s->writes};
}
int g1_prefetch(g1_store_t *s, uint32_t dim, int32_t x, int32_t z) {
    if (!owned(s))
        return -1;
    if (s->backend != G1_NATIVE)
        return 0;
    int32_t rx, rz;
    unsigned slot;
    mc_region_coordinates(x, z, &rx, &rz, &slot);
    struct g1_region *r = NULL;
    int rc = load_region(s, dim, rx, rz, &r);
    if (rc)
        return rc;
    uint8_t *e = r->index + 128 + slot * 32;
    if (!g1_u64(e))
        return 1;
    int error = posix_fadvise(r->fd, (off_t)g1_u64(e), (off_t)g1_u32(e + 8), POSIX_FADV_WILLNEED);
    if (error) {
        errno = error;
        return -1;
    }
    return 0;
}

static int add_key(uint32_t **d, int32_t **x, int32_t **z, size_t *n, uint32_t dim, int32_t cx, int32_t cz) {
    if (*n >= 100000) {
        errno = EFBIG;
        return -1;
    }
    size_t bytes = (*n + 1) * sizeof(uint32_t);
    uint32_t *a = realloc(*d, bytes);
    if (!a)
        return -1;
    *d = a;
    int32_t *b = realloc(*x, bytes);
    if (!b)
        return -1;
    *x = b;
    b = realloc(*z, bytes);
    if (!b)
        return -1;
    *z = b;
    (*d)[*n] = dim;
    (*x)[*n] = cx;
    (*z)[*n] = cz;
    (*n)++;
    return 0;
}
static bool parse_index(const char *name, uint32_t *dim, int32_t *rx, int32_t *rz) {
    char canonical[128];
    int n = 0;
    if (sscanf(name, "d.%u.r.%d.%d.idx%n", dim, rx, rz, &n) != 3 || !n || name[n] || *dim > 2)
        return false;
    region_name(canonical, *dim, *rx, *rz, "idx");
    return !strcmp(name, canonical) && *rx >= INT32_MIN / 32 && *rx <= (INT32_MAX - 31) / 32 &&
           *rz >= INT32_MIN / 32 && *rz <= (INT32_MAX - 31) / 32;
}
int g1_keys(g1_store_t *s, uint32_t **ds, int32_t **xs, int32_t **zs, size_t *count) {
    if (!owned(s))
        return -1;
    *ds = NULL;
    *xs = *zs = NULL;
    *count = 0;
    if (s->backend == G1_SQLITE)
        return g1_sql_keys(s, ds, xs, zs, count);
    int rc = 0;
    unsigned dims = s->backend == G1_NATIVE ? 1 : 3;
    for (unsigned dim_loop = 0; dim_loop < dims && !rc; dim_loop++) {
        char sub[64];
        snprintf(sub, sizeof(sub), "anvil/d.%u", dim_loop);
        int dir_fd = s->backend == G1_NATIVE
                         ? openat(s->root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC)
                         : openat(s->root, sub, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (dir_fd < 0) {
            rc = -1;
            break;
        }
        DIR *dir = fdopendir(dir_fd);
        if (!dir) {
            close(dir_fd);
            rc = -1;
            break;
        }
        for (;;) {
            errno = 0;
            struct dirent *de = readdir(dir);
            if (!de) {
                if (errno)
                    rc = -1;
                break;
            }
            uint32_t dim;
            int32_t rx, rz;
            int chars = 0;
            if (s->backend == G1_NATIVE) {
                if (!parse_index(de->d_name, &dim, &rx, &rz)) {
                    const char *temporary = strstr(de->d_name, ".idx.tmp.");
                    if (temporary) {
                        char base[128];
                        size_t n = (size_t)(temporary - de->d_name) + 4;
                        if (n < sizeof(base)) {
                            memcpy(base, de->d_name, n);
                            base[n] = 0;
                            if (parse_index(base, &dim, &rx, &rz))
                                continue;
                        }
                    }

                    size_t name_n = strlen(de->d_name);
                    if (name_n > 6 && !strcmp(de->d_name + name_n - 6, ".owner")) {
                        char idx[128];
                        if (name_n >= sizeof(idx)) {
                            rc = bad();
                            break;
                        }
                        memcpy(idx, de->d_name, name_n - 6);
                        strcpy(idx + name_n - 6, ".idx");
                        struct stat owned;
                        if (fstatat(s->root, idx, &owned, AT_SYMLINK_NOFOLLOW)) {
                            rc = bad();
                            break;
                        }
                    }
                    if (strstr(de->d_name, ".idx") && de->d_name[0] != '.') {
                        rc = bad();
                        break;
                    }
                    continue;
                }
                struct g1_region *r = NULL;
                if (load_region(s, dim, rx, rz, &r)) {
                    rc = -1;
                    break;
                }
                for (unsigned slot = 0; slot < 1024; slot++)
                    if (g1_u64(r->index + 128 + slot * 32) &&
                        add_key(ds, xs, zs, count, dim, rx * 32 + (int32_t)(slot % 32),
                                rz * 32 + (int32_t)(slot / 32))) {
                        rc = -1;
                        break;
                    }
            } else {
                dim = dim_loop;
                if (sscanf(de->d_name, "r.%d.%d.mca%n", &rx, &rz, &chars) != 2 || !chars || de->d_name[chars])
                    continue;
                if (rx < INT32_MIN / 32 || rx > (INT32_MAX - 31) / 32 || rz < INT32_MIN / 32 ||
                    rz > (INT32_MAX - 31) / 32) {
                    rc = bad();
                    break;
                }
                char path[4096];
                mc_region_file_t *file = NULL;
                if (anvil_path(s, dim, rx, rz, path) || mc_region_open(path, &file)) {
                    rc = -1;
                    break;
                }
                for (unsigned slot = 0; slot < 1024; slot++)
                    if (mc_region_entry(mc_region_file_index(file), slot)->sectors &&
                        add_key(ds, xs, zs, count, dim, rx * 32 + (int32_t)(slot % 32),
                                rz * 32 + (int32_t)(slot / 32))) {
                        rc = -1;
                        break;
                    }
                if (mc_region_close(file))
                    rc = -1;
            }
            if (rc)
                break;
        }
        if (closedir(dir))
            rc = -1;
    }
    if (rc) {
        free(*ds);
        free(*xs);
        free(*zs);
        *ds = NULL;
        *xs = *zs = NULL;
        *count = 0;
    }
    return rc;
}
int g1_verify(g1_store_t *s, size_t *count) {
    if (!owned(s))
        return -1;
    if (s->backend == G1_SQLITE && g1_sql_check(s))
        return -1;
    invalidate(s);
    evict_regions(s);
    uint32_t *d = NULL;
    int32_t *x = NULL, *z = NULL;
    size_t n = 0;
    if (g1_keys(s, &d, &x, &z, &n))
        return -1;
    int rc = 0;
    size_t saved = s->cache_limit;
    s->cache_limit = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t *raw = NULL;
        size_t len = 0;
        uint64_t gen;
        if (g1_read(s, d[i], x[i], z[i], &raw, &len, &gen) || g1_validate_nbt(raw, len, x[i], z[i]))
            rc = -1;
        free(raw);
        if (rc)
            break;
    }
    s->cache_limit = saved;
    free(d);
    free(x);
    free(z);
    if (!rc)
        *count = n;
    else
        s->poisoned = true;
    return rc;
}
static int compact_region(g1_store_t *s, uint32_t dim, int32_t rx, int32_t rz) {
    struct g1_region *r = NULL;
    if (load_region(s, dim, rx, rz, &r))
        return -1;
    uint8_t *next = malloc(G1_INDEX_BYTES);
    if (!next)
        return -1;
    memcpy(next, r->index, G1_INDEX_BYTES);
    uint64_t previous = g1_u64(next + 40), id;
    if (g1_u64(next + 32) == UINT64_MAX) {
        free(next);
        errno = EOVERFLOW;
        return -1;
    }
    int fd = new_data(s, dim, rx, rz, &id);
    if (fd < 0) {
        free(next);
        return -1;
    }
    int rc = 0;
    uint64_t end = 4096;
    G1_HOOK("compact-copy");
    for (unsigned slot = 0; slot < 1024; slot++) {
        uint8_t *e = next + 128 + slot * 32;
        if (!g1_u64(e))
            continue;
        uint8_t *raw = NULL;
        size_t raw_n = 0;
        uint64_t gen;
        if (native_read(s, dim, rx * 32 + (int32_t)(slot % 32), rz * 32 + (int32_t)(slot / 32), &raw, &raw_n,
                        &gen)) {
            rc = -1;
            break;
        }
        free(raw);
        size_t len = g1_u32(e + 8);
        uint8_t *data = malloc(len);
        if (!data) {
            rc = -1;
            break;
        }
        rc = mc_file_pread_all(r->fd, data, len, (off_t)g1_u64(e));
        if (!rc)
            rc = mc_file_pwrite_all(fd, data, len, (off_t)end);
        free(data);
        if (rc)
            break;
        g1_p64(e, end);
        end += len;
    }
    if (!rc)
        rc = g1_sync(fd);
    rc = close_fd(fd, rc);
    if (!rc)
        rc = g1_sync(s->root);
    G1_HOOK("compact-data-durable");
    if (!rc) {
        g1_p64(next + 32, g1_u64(next + 32) + 1);
        g1_p64(next + 40, id);
        g1_p64(next + 48, end);
        rc = publish(s, dim, rx, rz, next);
    }
    free(next);
    if (!rc) {
        G1_HOOK("compact-published");
        char old[128];
        data_name(old, dim, rx, rz, previous);
        G1_HOOK("compact-cleanup");
        if (unlinkat(s->root, old, 0) || g1_sync(s->root))
            rc = -3;
    }
    evict_regions(s);
    invalidate(s);
    return rc;
}
static int collect_orphans(g1_store_t *s) {
    /* An observed root is first made durable again. Only recognized, checksummed
     * old data of this world are reclaimed. Atomic index temps stay inspectable. */
    DIR *dir = fdopendir(openat(s->root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!dir)
        return -1;
    int rc = 0;
    for (;;) {
        errno = 0;
        struct dirent *de = readdir(dir);
        if (!de) {
            if (errno)
                rc = -1;
            break;
        }
        uint32_t dim;
        int32_t rx, rz;
        uint64_t id;
        int chars = 0;
        char canonical[128];
        if (sscanf(de->d_name, "d.%u.r.%d.%d.data.%16" SCNx64 "%n", &dim, &rx, &rz, &id, &chars) != 4 ||
            !chars || de->d_name[chars] || dim > 2)
            continue;
        data_name(canonical, dim, rx, rz, id);
        if (strcmp(canonical, de->d_name))
            continue;
        struct g1_region *r = NULL;
        int found = load_region(s, dim, rx, rz, &r);
        if (found == 1)
            continue;
        if (found < 0) {
            rc = -1;
            break;
        }
        if (g1_u64(r->index + 40) == id)
            continue;
        int fd = openat(s->root, de->d_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        uint8_t h[128];
        struct stat st;
        if (fd < 0) {
            rc = -1;
            break;
        }
        bool owned = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_nlink == 1 && st.st_size >= 128 &&
                     !mc_file_pread_all(fd, h, 128, 0) && valid_data(h, s, dim, rx, rz, id);
        if (close_fd(fd, 0)) {
            rc = -1;
            break;
        }
        if (!owned)
            continue;
        char idx[128];
        region_name(idx, dim, rx, rz, "idx");
        int index = openat(s->root, idx, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (index < 0) {
            rc = -1;
            break;
        }
        rc = g1_sync(r->fd);
        if (!rc)
            rc = g1_sync(index);
        rc = close_fd(index, rc);
        if (!rc)
            rc = g1_sync(s->root);
        if (!rc && (unlinkat(s->root, de->d_name, 0) || g1_sync(s->root)))
            rc = -1;
        if (rc)
            break;
    }
    if (closedir(dir))
        rc = -1;
    return rc;
}
int g1_compact(g1_store_t *s) {
    if (!owned(s))
        return -1;
    if (!s->writable || s->poisoned) {
        errno = EPERM;
        return -1;
    }
    if (s->backend == G1_SQLITE)
        return g1_sql_finish(s, true);
    if (s->backend == G1_ANVIL)
        return 0;
    size_t count = 0;
    if (g1_verify(s, &count))
        return -1;
    uint32_t *d = NULL;
    int32_t *x = NULL, *z = NULL;
    size_t n = 0;
    if (g1_keys(s, &d, &x, &z, &n))
        return -1;
    int rc = 0;
    /* Keys need not be in file order: compact each distinct region once. */
    for (size_t i = 0; i < n; i++) {
        int32_t rx, rz;
        mc_region_coordinates(x[i], z[i], &rx, &rz, NULL);
        bool seen = false;
        for (size_t j = 0; j < i; j++) {
            int32_t a, b;
            mc_region_coordinates(x[j], z[j], &a, &b, NULL);
            if (d[i] == d[j] && a == rx && b == rz) {
                seen = true;
                break;
            }
        }
        if (!seen && (rc = compact_region(s, d[i], rx, rz)))
            break;
    }
    free(d);
    free(x);
    free(z);
    if (!rc)
        rc = collect_orphans(s);
    if (rc)
        s->poisoned = true;
    return rc;
}
