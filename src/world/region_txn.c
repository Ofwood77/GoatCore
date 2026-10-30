#include "mc_region_txn.h"
#include "mc_atomic_file.h"
#include "mc_util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#define STATE_BYTES 160u
#define JOURNAL_PREFIX 128u
#define HEADER_OFFSET JOURNAL_PREFIX
#define NEW_HEADER_OFFSET (JOURNAL_PREFIX + MC_REGION_HEADER_BYTES)
#define HASH_OFFSET (JOURNAL_PREFIX + 2u * MC_REGION_HEADER_BYTES)
#define HASH_RECORD 40u
#define JOURNAL_MAX (HASH_OFFSET + 2u * MC_REGION_SLOTS * HASH_RECORD + 32u)
#define BATCH_MAX (64u * 1024u * 1024u)

#ifdef MC_REGION_TXN_TESTING
extern void mc_region_txn_test_hook(const char *stage);
#define HOOK(s) mc_region_txn_test_hook(s)
#else
#define HOOK(s) ((void)0)
#endif

struct mc_region_txn_store {
    int dir, owner;
    bool failed;
    pthread_t thread;
};
typedef struct {
    unsigned slot;
    uint8_t *record;
    size_t length;
    mc_region_reservation_t allocation;
} prepared_t;

static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint64_t u64(const uint8_t *p) {
    return (uint64_t)u32(p) << 32 | u32(p + 4);
}
static void put32(uint8_t *p, uint32_t n) {
    p[0] = n >> 24;
    p[1] = n >> 16;
    p[2] = n >> 8;
    p[3] = n;
}
static void put64(uint8_t *p, uint64_t n) {
    put32(p, n >> 32);
    put32(p + 4, n);
}
static int sync_fd(int fd) {
    int r;
    do {
        r = fsync(fd);
    } while (r < 0 && errno == EINTR);
    return r;
}
static int stat_fd(int fd, struct stat *s) {
    int r;
    do {
        r = fstat(fd, s);
    } while (r < 0 && errno == EINTR);
    return r;
}
static int resize(int fd, uint64_t n) {
    int r;
    do {
        r = ftruncate(fd, (off_t)n);
    } while (r < 0 && errno == EINTR);
    return r;
}
static int remove_at(int dir, const char *name) {
    int r;
    do {
        r = unlinkat(dir, name, 0);
    } while (r < 0 && errno == EINTR);
    return r;
}
static int open_at(int dir, const char *name, int flags) {
    int r;
    do {
        r = openat(dir, name, flags | O_CLOEXEC | O_NOFOLLOW, 0600);
    } while (r < 0 && errno == EINTR);
    return r;
}
static int digest(const void *data, size_t n, uint8_t out[32]) {
    unsigned size = 0;
    if (EVP_Digest(data, n, out, &size, EVP_sha256(), NULL) != 1 || size != 32) {
        errno = EIO;
        return -1;
    }
    return 0;
}
static int digest_record(int fd, const mc_region_entry_t *e, uint8_t out[32]) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        errno = ENOMEM;
        return -1;
    }
    int rc = -1;
    uint8_t buf[65536];
    unsigned size = 0;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
        errno = EIO;
        goto done;
    }
    size_t left = (size_t)e->length + 4;
    off_t pos = (off_t)e->offset * 4096;
    while (left) {
        size_t n = left > sizeof(buf) ? sizeof(buf) : left;
        if (mc_file_pread_all(fd, buf, n, pos) != 0)
            goto done;
        if (EVP_DigestUpdate(ctx, buf, n) != 1) {
            errno = EIO;
            goto done;
        }
        pos += (off_t)n;
        left -= n;
    }
    if (EVP_DigestFinal_ex(ctx, out, &size) != 1 || size != 32) {
        errno = EIO;
        goto done;
    }
    rc = 0;
done:
    EVP_MD_CTX_free(ctx);
    return rc;
}
static bool zeros(const uint8_t *p, size_t n) {
    while (n--)
        if (*p++)
            return false;
    return true;
}
static void names(int32_t rx, int32_t rz, char region[96], char state[128], char journal[128]) {
    snprintf(region, 96, "r.%d.%d.mca", rx, rz);
    snprintf(state, 128, "%s.s2-state", region);
    snprintf(journal, 128, "%s.s2-journal", region);
}
static int atomic_replace(mc_region_txn_store_t *s, const char *name, const uint8_t *data, size_t n,
                          bool journal) {
    char path[192];
    snprintf(path, sizeof(path), "/proc/self/fd/%d/%s", s->dir, name);
    mc_atomic_file_t file = {.dir_fd = -1, .temp_fd = -1, .path = path};
    if (mc_atomic_file_begin(&file, path) != 0)
        return -1;
    int rc = -1;
    if (journal) {
        if (mc_file_write_all(file.temp_fd, data, JOURNAL_PREFIX) != 0)
            goto done;
        HOOK("journal_partial");
        if (mc_file_write_all(file.temp_fd, data + JOURNAL_PREFIX, n - JOURNAL_PREFIX) != 0)
            goto done;
    } else if (mc_file_write_all(file.temp_fd, data, n) != 0)
        goto done;
    rc = mc_atomic_file_commit(&file);
done:;
    int saved = errno;
    mc_atomic_file_abort(&file);
    errno = saved;
    return rc;
}
static int read_blob(mc_region_txn_store_t *s, const char *name, uint8_t **out, size_t *size, size_t max) {
    *out = NULL;
    *size = 0;
    int fd = open_at(s->dir, name, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT ? 1 : -1;
    struct stat st;
    int rc = -1;
    uint8_t *data = NULL;
    if (stat_fd(fd, &st) != 0)
        goto done;
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size < 0 || (uint64_t)st.st_size > max) {
        errno = EBADMSG;
        goto done;
    }
    data = malloc(st.st_size ? (size_t)st.st_size : 1);
    if (!data || mc_file_pread_all(fd, data, (size_t)st.st_size, 0) != 0)
        goto done;
    *size = (size_t)st.st_size;
    *out = data;
    data = NULL;
    rc = 0;
done:;
    int saved = errno;
    if (close(fd) != 0 && rc == 0) {
        rc = -1;
        saved = errno;
        free(*out);
        *out = NULL;
        *size = 0;
    }
    free(data);
    errno = saved;
    return rc;
}
static int check_state(const uint8_t *p, size_t n, int32_t rx, int32_t rz, const struct stat *st) {
    uint8_t hash[32];
    if (n != STATE_BYTES || memcmp(p, "GCS2STA1", 8) || u32(p + 8) != 1 || u32(p + 12) != STATE_BYTES ||
        u64(p + 40) != (uint64_t)st->st_dev || u64(p + 48) != (uint64_t)st->st_ino ||
        u32(p + 56) != (uint32_t)rx || u32(p + 60) != (uint32_t)rz || !zeros(p + 68, 28) ||
        (u64(p + 32) == 0 ? u32(p + 64) != 0 || !zeros(p + 96, 32) : u32(p + 64) < 1 || u32(p + 64) > 2)) {
        errno = EBADMSG;
        return -1;
    }
    if (digest(p, 128, hash) != 0)
        return -1;
    if (memcmp(hash, p + 128, 32)) {
        errno = EBADMSG;
        return -1;
    }
    return 0;
}
static int initialize_state(mc_region_txn_store_t *s, const char *name, int32_t rx, int32_t rz,
                            const struct stat *st, uint8_t state[STATE_BYTES]) {
    memset(state, 0, STATE_BYTES);
    memcpy(state, "GCS2STA1", 8);
    put32(state + 8, 1);
    put32(state + 12, STATE_BYTES);
    size_t pos = 0;
    while (pos < 16) {
        ssize_t n = getrandom(state + 16 + pos, 16 - pos, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (!n)
                errno = EIO;
            return -1;
        }
        pos += (size_t)n;
    }
    put64(state + 40, (uint64_t)st->st_dev);
    put64(state + 48, (uint64_t)st->st_ino);
    put32(state + 56, (uint32_t)rx);
    put32(state + 60, (uint32_t)rz);
    if (digest(state, 128, state + 128) != 0)
        return -1;
    return atomic_replace(s, name, state, STATE_BYTES, false);
}
static int lock_region(mc_region_txn_store_t *s, const char *region) {
    char name[128];
    snprintf(name, sizeof(name), "%s.lock", region);
    int fd = open_at(s->dir, name, O_RDWR | O_CREAT);
    if (fd < 0)
        return -1;
    struct stat st;
    int rc;
    if (stat_fd(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
        int e = errno ? errno : EINVAL;
        close(fd);
        errno = e;
        return -1;
    }
    do {
        rc = flock(fd, LOCK_EX | LOCK_NB);
    } while (rc < 0 && errno == EINTR);
    if (rc != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

/* Full immutable journal + all old live records are checked BEFORE changing a
 * byte. Current header is intentionally untrusted (may be arbitrarily torn). */
static int validate_journal(int fd, const uint8_t *j, size_t n, const uint8_t *state, int32_t rx, int32_t rz,
                            mc_region_index_t **old) {
    *old = NULL;
    uint8_t hash[32];
    struct stat st;
    if (n < HASH_OFFSET + 32 || memcmp(j, "GCS2UND1", 8) || u32(j + 8) != 1 || u32(j + 12) != n ||
        memcmp(j + 16, state + 16, 16) || !u64(j + 32) || u64(j + 40) != u64(state + 40) ||
        u64(j + 48) != u64(state + 48) || u32(j + 72) != (uint32_t)rx || u32(j + 76) != (uint32_t)rz ||
        !zeros(j + 88, 40)) {
        errno = EBADMSG;
        return -1;
    }
    uint32_t old_count = u32(j + 80), changed = u32(j + 84);
    uint64_t old_size = u64(j + 56), new_size = u64(j + 64);
    if (old_count > 1024 || !changed || changed > 1024 ||
        n != HASH_OFFSET + (size_t)(old_count + changed) * HASH_RECORD + 32 || old_size < 8192 ||
        new_size < old_size || new_size > (uint64_t)MC_REGION_MAX_SECTORS * 4096 || old_size % 4096 ||
        new_size % 4096) {
        errno = EBADMSG;
        return -1;
    }
    if (digest(j, n - 32, hash) != 0)
        return -1;
    if (memcmp(hash, j + n - 32, 32)) {
        errno = EBADMSG;
        return -1;
    }
    uint64_t generation = u64(state + 32), transaction = u64(j + 32);
    if (transaction == generation) {
        if (memcmp(state + 96, j + n - 32, 32)) {
            errno = EBADMSG;
            return -1;
        }
    } else if (generation == UINT64_MAX || transaction != generation + 1) {
        errno = EBADMSG;
        return -1;
    }
    if (stat_fd(fd, &st) != 0)
        return -1;
    if ((uint64_t)st.st_size < old_size || (uint64_t)st.st_size > new_size) {
        errno = EBADMSG;
        return -1;
    }
    if (mc_region_index_load_snapshot(fd, j + HEADER_OFFSET, old_size, old) != 0)
        return -1;
    const mc_region_stats_t *stats = mc_region_stats(*old);
    if (stats->chunks != old_count || stats->external || stats->unsupported) {
        errno = EBADMSG;
        goto bad;
    }
    const uint8_t *p = j + HASH_OFFSET;
    for (unsigned slot = 0; slot < 1024; slot++) {
        const mc_region_entry_t *e = mc_region_entry(*old, slot);
        if (!e->offset)
            continue;
        if (u32(p) != slot || u32(p + 4) != e->length + 4) {
            errno = EBADMSG;
            goto bad;
        }
        if (digest_record(fd, e, hash) != 0)
            goto bad;
        if (memcmp(hash, p + 8, 32)) {
            errno = EBADMSG;
            goto bad;
        }
        p += HASH_RECORD;
    }
    mc_region_plan_t *plan = NULL;
    if (mc_region_plan_create(*old, &plan) != 0)
        goto bad;
    bool seen[1024] = {0};
    unsigned previous = 0;
    for (unsigned i = 0; i < changed; i++, p += HASH_RECORD) {
        unsigned slot = u32(p);
        uint32_t length = u32(p + 4);
        if (slot >= 1024 || (i && slot <= previous)) {
            errno = EBADMSG;
            goto plan_bad;
        }
        previous = slot;
        seen[slot] = true;
        uint32_t raw = u32(j + NEW_HEADER_OFFSET + slot * 4), off = raw >> 8, count = raw & 255;
        mc_region_reservation_t reservation;
        if (length < 5 || (uint64_t)length > (uint64_t)count * 4096 ||
            (uint64_t)off + count > new_size / 4096 ||
            mc_region_plan_reserve_at(plan, off, count, &reservation) != 0) {
            errno = EBADMSG;
            goto plan_bad;
        }
    }
    for (unsigned slot = 0; slot < 1024; slot++)
        if (!seen[slot]) {
            if (memcmp(j + HEADER_OFFSET + slot * 4, j + NEW_HEADER_OFFSET + slot * 4, 4) ||
                memcmp(j + HEADER_OFFSET + 4096 + slot * 4, j + NEW_HEADER_OFFSET + 4096 + slot * 4, 4)) {
                errno = EBADMSG;
                goto plan_bad;
            }
        }
    mc_region_plan_free(plan);
    return 0;
plan_bad:
    mc_region_plan_free(plan);
bad:;
    int e = errno;
    mc_region_index_free(*old);
    *old = NULL;
    errno = e;
    return -1;
}
static int verify_new(int fd, const uint8_t *j, size_t n) {
    mc_region_index_t *index = NULL;
    if (mc_region_index_load_snapshot(fd, j + NEW_HEADER_OFFSET, u64(j + 64), &index) != 0)
        return -1;
    uint8_t hash[32];
    int rc = -1;
    const uint8_t *p = j + HASH_OFFSET + (size_t)u32(j + 80) * HASH_RECORD;
    for (unsigned i = 0; i < u32(j + 84); i++, p += HASH_RECORD) {
        const mc_region_entry_t *e = mc_region_entry(index, u32(p));
        if (!e || !e->decodable || e->compression != 2 || e->length + 4 != u32(p + 4)) {
            errno = EBADMSG;
            goto done;
        }
        if (digest_record(fd, e, hash) != 0)
            goto done;
        if (memcmp(hash, p + 8, 32)) {
            errno = EBADMSG;
            goto done;
        }
    }
    (void)n;
    rc = 0;
done:;
    int saved = errno;
    mc_region_index_free(index);
    errno = saved;
    return rc;
}
static int finish_state(mc_region_txn_store_t *s, const char *name, uint8_t state[STATE_BYTES],
                        const uint8_t *journal, size_t n, uint32_t outcome) {
    put64(state + 32, u64(journal + 32));
    put32(state + 64, outcome);
    memcpy(state + 96, journal + n - 32, 32);
    if (digest(state, 128, state + 128) != 0)
        return -1;
    return atomic_replace(s, name, state, STATE_BYTES, false);
}
static int recover_one(mc_region_txn_store_t *s, int32_t rx, int32_t rz) {
    char region[96], state_name[128], journal[128];
    names(rx, rz, region, state_name, journal);
    uint8_t *j = NULL, *state = NULL;
    size_t n = 0, sn = 0;
    int rc = read_blob(s, journal, &j, &n, JOURNAL_MAX);
    if (rc == 1)
        return 0;
    if (rc != 0)
        return -1;
    int lock = -1, fd = -1;
    mc_region_index_t *old = NULL;
    rc = -1;
    lock = lock_region(s, region);
    if (lock < 0)
        goto done;
    fd = open_at(s->dir, region, O_RDWR);
    if (fd < 0)
        goto done;
    struct stat st;
    if (stat_fd(fd, &st) != 0)
        goto done;
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1) {
        errno = EBADMSG;
        goto done;
    }
    int state_rc = read_blob(s, state_name, &state, &sn, STATE_BYTES);
    if (state_rc != 0) {
        if (state_rc == 1)
            errno = EBADMSG;
        goto done;
    }
    if (check_state(state, sn, rx, rz, &st) != 0 || validate_journal(fd, j, n, state, rx, rz, &old) != 0)
        goto done;
    bool committed = u64(state + 32) == u64(j + 32) && u32(state + 64) == 1;
    if (committed && verify_new(fd, j, n) != 0)
        goto done;
    HOOK("recovery_begin");
    HOOK("recovery_header_begin");
    if (mc_file_pwrite_all(fd, j + (committed ? NEW_HEADER_OFFSET : HEADER_OFFSET), 8192, 0) != 0) {
        rc = -2;
        goto done;
    }
    HOOK("recovery_header_written");
    /* Do not truncate an uncommitted tail; align only a partial growth upward. */
    uint64_t aligned = ((uint64_t)st.st_size + 4095) / 4096 * 4096;
    if (aligned != (uint64_t)st.st_size && resize(fd, aligned) != 0) {
        rc = -2;
        goto done;
    }
    if (sync_fd(fd) != 0) {
        rc = -2;
        goto done;
    }
    HOOK("recovery_synced");
    if (u64(state + 32) != u64(j + 32) && finish_state(s, state_name, state, j, n, 2) != 0) {
        rc = -2;
        goto done;
    }
    HOOK("recovery_state_durable");
    {
        int f = fd;
        fd = -1;
        if (close(f) != 0) {
            rc = -2;
            goto done;
        }
    }
    if (remove_at(s->dir, journal) != 0) {
        rc = -2;
        goto done;
    }
    HOOK("recovery_unlinked");
    if (sync_fd(s->dir) != 0) {
        rc = -2;
        goto done;
    }
    HOOK("recovery_complete");
    rc = 0;
done:;
    int e = errno;
    if (fd >= 0 && close(fd) != 0 && rc == 0) {
        rc = -2;
        e = errno;
    }
    if (lock >= 0 && close(lock) != 0 && rc == 0) {
        rc = -2;
        e = errno;
    }
    if (rc != 0)
        log_error("experimental Anvil recovery blocked region=(%d,%d) journal=%s errno=%d", rx, rz, journal,
                  e);
    mc_region_index_free(old);
    free(j);
    free(state);
    errno = e;
    return rc;
}
static int parse_journal_name(const char *name, int32_t *rx, int32_t *rz) {
    char a[96], b[128], canonical[128], *end;
    if (strncmp(name, "r.", 2))
        return -1;
    errno = 0;
    long x = strtol(name + 2, &end, 10);
    if (errno || end == name + 2 || *end != '.' || x < INT32_MIN || x > INT32_MAX)
        return -1;
    const char *start = end + 1;
    errno = 0;
    long z = strtol(start, &end, 10);
    if (errno || end == start || strcmp(end, ".mca.s2-journal") || z < INT32_MIN || z > INT32_MAX)
        return -1;
    names((int32_t)x, (int32_t)z, a, b, canonical);
    if (strcmp(name, canonical))
        return -1;
    *rx = (int32_t)x;
    *rz = (int32_t)z;
    return 0;
}
int mc_region_txn_open(const char *directory, unsigned opt_in, mc_region_txn_store_t **out) {
    if (out)
        *out = NULL;
    if (!directory || !out || opt_in != MC_REGION_TXN_DISPOSABLE) {
        errno = EACCES;
        return -1;
    }
    mc_region_txn_store_t *s = calloc(1, sizeof(*s));
    if (!s)
        return -1;
    s->dir = s->owner = -1;
    s->thread = pthread_self();
    do {
        s->dir = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    } while (s->dir < 0 && errno == EINTR);
    if (s->dir < 0)
        goto bad;
    uint8_t *marker = NULL;
    size_t mn = 0;
    if (read_blob(s, MC_REGION_TXN_MARKER, &marker, &mn, sizeof(MC_REGION_TXN_MARKER_TEXT)) != 0)
        goto bad;
    bool valid = mn == strlen(MC_REGION_TXN_MARKER_TEXT) && !memcmp(marker, MC_REGION_TXN_MARKER_TEXT, mn);
    free(marker);
    if (!valid) {
        errno = EACCES;
        goto bad;
    }
    s->owner = open_at(s->dir, ".s2-owner.lock", O_CREAT | O_RDWR);
    if (s->owner < 0)
        goto bad;
    struct stat st;
    int r;
    if (stat_fd(s->owner, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
        errno = EINVAL;
        goto bad;
    }
    do {
        r = flock(s->owner, LOCK_EX | LOCK_NB);
    } while (r < 0 && errno == EINTR);
    if (r != 0 || sync_fd(s->dir) != 0)
        goto bad;
    int duplicate = dup(s->dir);
    if (duplicate < 0)
        goto bad;
    DIR *dir = fdopendir(duplicate);
    if (!dir) {
        int e = errno;
        close(duplicate);
        errno = e;
        goto bad;
    }
    struct dirent *entry;
    int scan_error = 0;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) {
            scan_error = errno;
            break;
        }
        size_t len = strlen(entry->d_name);
        if (len < 11 || strcmp(entry->d_name + len - 11, ".s2-journal"))
            continue;
        int32_t rx, rz;
        if (parse_journal_name(entry->d_name, &rx, &rz) != 0) {
            scan_error = EBADMSG;
            break;
        }
        if (recover_one(s, rx, rz) != 0) {
            scan_error = errno ? errno : EIO;
            break;
        }
    }
    if (closedir(dir) != 0 && !scan_error)
        scan_error = errno;
    if (scan_error) {
        errno = scan_error;
        goto bad;
    }
    *out = s;
    return 0;
bad:;
    int e = errno;
    mc_region_txn_close(s);
    errno = e;
    return -1;
}
int mc_region_txn_close(mc_region_txn_store_t *s) {
    if (!s)
        return 0;
    if (!pthread_equal(s->thread, pthread_self())) {
        errno = EPERM;
        return -1;
    }
    int rc = 0, e = errno;
    if (s->owner >= 0 && close(s->owner) != 0) {
        rc = -1;
        e = errno;
    }
    if (s->dir >= 0 && close(s->dir) != 0) {
        rc = -1;
        e = errno;
    }
    free(s);
    errno = e;
    return rc;
}
static int compare_prepared(const void *a, const void *b) {
    const prepared_t *x = a, *y = b;
    return (x->slot > y->slot) - (x->slot < y->slot);
}
int mc_region_txn_write(mc_region_txn_store_t *s, int32_t rx, int32_t rz, const mc_region_update_t *updates,
                        size_t count) {
    if (s && !pthread_equal(s->thread, pthread_self())) {
        errno = EPERM;
        return -1;
    }
    if (!s || s->failed || !updates || !count || count > 1024) {
        errno = EINVAL;
        return -1;
    }
    int rc = -1, fd = -1, lock = -1;
    bool journal_live = false;
    prepared_t *p = calloc(count, sizeof(*p));
    uint8_t *journal_data = NULL, *state_data = NULL;
    mc_region_index_t *index = NULL;
    mc_region_plan_t *plan = NULL;
    uint8_t state[STATE_BYTES];
    size_t sn = 0;
    char region[96], state_name[128], journal[128];
    names(rx, rz, region, state_name, journal);
    if (!p)
        goto done;
    size_t batch = 0;
    mc_save_clock_t span = mc_save_profile_begin();
    for (size_t i = 0; i < count; i++) {
        if (updates[i].slot >= 1024 || !updates[i].nbt || !updates[i].length ||
            updates[i].length > BATCH_MAX) {
            errno = EINVAL;
            goto done;
        }
        p[i].slot = updates[i].slot;
        uLongf bound = compressBound(updates[i].length);
        uint8_t *compressed = malloc(bound);
        if (!compressed)
            goto done;
        int z = compress2(compressed, &bound, updates[i].nbt, updates[i].length, Z_DEFAULT_COMPRESSION);
        if (z != Z_OK) {
            free(compressed);
            errno = z == Z_MEM_ERROR ? ENOMEM : EIO;
            goto done;
        }
        size_t sectors = ((size_t)bound + 5 + 4095) / 4096;
        if (sectors > 255 || batch + sectors * 4096 > BATCH_MAX) {
            free(compressed);
            errno = EFBIG;
            goto done;
        }
        p[i].length = (size_t)bound + 5;
        p[i].record = calloc(sectors, 4096);
        if (!p[i].record) {
            free(compressed);
            goto done;
        }
        put32(p[i].record, (uint32_t)bound + 1);
        p[i].record[4] = 2;
        memcpy(p[i].record + 5, compressed, bound);
        free(compressed);
        batch += sectors * 4096;
    }
    qsort(p, count, sizeof(*p), compare_prepared);
    for (size_t i = 1; i < count; i++)
        if (p[i - 1].slot == p[i].slot) {
            errno = EINVAL;
            goto done;
        }
    mc_save_profile_end("s2_compress", rx, rz, span, batch, 0);
    lock = lock_region(s, region);
    if (lock < 0)
        goto done;
    /* This handle begins clean; an unexpected journal must never be overwritten. */
    struct stat st;
    int sr;
    do {
        sr = fstatat(s->dir, journal, &st, AT_SYMLINK_NOFOLLOW);
    } while (sr < 0 && errno == EINTR);
    if (sr == 0 || errno != ENOENT) {
        if (sr == 0)
            errno = EBUSY;
        goto done;
    }
    fd = open_at(s->dir, region, O_RDWR);
    if (fd < 0 && errno == ENOENT) {
        uint8_t empty[8192] = {0};
        rc = atomic_replace(s, region, empty, sizeof(empty), false);
        if (rc != 0)
            goto done;
        rc = -1;
        fd = open_at(s->dir, region, O_RDWR);
    }
    if (fd < 0 || stat_fd(fd, &st) != 0)
        goto done;
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1) {
        errno = EINVAL;
        goto done;
    }
    HOOK("prepare_begin");
    span = mc_save_profile_begin();
    if (mc_region_index_load(fd, &index) != 0 || mc_region_plan_create(index, &plan) != 0)
        goto done;
    const mc_region_stats_t *stats = mc_region_stats(index);
    for (size_t i = 0; i < count; i++)
        if (mc_region_plan_reserve(plan, (uint32_t)((p[i].length + 4095) / 4096), &p[i].allocation) != 0)
            goto done;
    size_t journal_size = HASH_OFFSET + (stats->chunks + count) * HASH_RECORD + 32;
    journal_data = calloc(1, journal_size);
    if (!journal_data)
        goto done;
    memcpy(journal_data, "GCS2UND1", 8);
    put32(journal_data + 8, 1);
    put32(journal_data + 12, (uint32_t)journal_size);
    put64(journal_data + 40, (uint64_t)st.st_dev);
    put64(journal_data + 48, (uint64_t)st.st_ino);
    put64(journal_data + 56, stats->file_bytes);
    put64(journal_data + 64, (uint64_t)mc_region_plan_sectors(plan) * 4096);
    put32(journal_data + 72, (uint32_t)rx);
    put32(journal_data + 76, (uint32_t)rz);
    put32(journal_data + 80, stats->chunks);
    put32(journal_data + 84, (uint32_t)count);
    memcpy(journal_data + HEADER_OFFSET, mc_region_header(index), 8192);
    memcpy(journal_data + NEW_HEADER_OFFSET, mc_region_header(index), 8192);
    uint8_t *hash = journal_data + HASH_OFFSET;
    for (unsigned slot = 0; slot < 1024; slot++) {
        const mc_region_entry_t *e = mc_region_entry(index, slot);
        if (!e->offset)
            continue;
        put32(hash, slot);
        put32(hash + 4, e->length + 4);
        if (digest_record(fd, e, hash + 8) != 0)
            goto done;
        hash += HASH_RECORD;
    }
    time_t timestamp = time(NULL);
    if (timestamp == (time_t)-1)
        goto done;
    for (size_t i = 0; i < count; i++) {
        unsigned slot = p[i].slot;
        mc_region_reservation_t a = p[i].allocation;
        put32(journal_data + NEW_HEADER_OFFSET + slot * 4, a.offset << 8 | a.sectors);
        put32(journal_data + NEW_HEADER_OFFSET + 4096 + slot * 4, (uint32_t)timestamp);
        put32(hash, slot);
        put32(hash + 4, (uint32_t)p[i].length);
        if (digest(p[i].record, p[i].length, hash + 8) != 0)
            goto done;
        hash += HASH_RECORD;
    }
    mc_save_profile_end("s2_prepare_hash", rx, rz, span, 0, 0);
    int state_rc = read_blob(s, state_name, &state_data, &sn, STATE_BYTES);
    if (state_rc == 1) {
        HOOK("state_begin");
        rc = initialize_state(s, state_name, rx, rz, &st, state);
        if (rc != 0)
            goto done;
        rc = -1;
    } else {
        if (state_rc != 0 || check_state(state_data, sn, rx, rz, &st) != 0)
            goto done;
        memcpy(state, state_data, STATE_BYTES);
    }
    if (u64(state + 32) == UINT64_MAX) {
        errno = EOVERFLOW;
        goto done;
    }
    memcpy(journal_data + 16, state + 16, 16);
    put64(journal_data + 32, u64(state + 32) + 1);
    if (digest(journal_data, journal_size - 32, journal_data + journal_size - 32) != 0)
        goto done;
    HOOK("before_journal");
    span = mc_save_profile_begin();
    rc = atomic_replace(s, journal, journal_data, journal_size, true);
    if (rc != 0) {
        if (rc == -2)
            journal_live = true;
        goto done;
    }
    journal_live = true;
    rc = -2;
    mc_save_profile_end("s2_journal", rx, rz, span, journal_size, 0);
    HOOK("journal_durable");
    uint64_t new_size = u64(journal_data + 64);
    if (new_size > stats->file_bytes && resize(fd, new_size) != 0)
        goto done;
    HOOK("data_begin");
    span = mc_save_profile_begin();
    for (size_t i = 0; i < count; i++)
        if (mc_file_pwrite_all(fd, p[i].record, (size_t)p[i].allocation.sectors * 4096,
                               (off_t)p[i].allocation.offset * 4096) != 0)
            goto done;
    mc_save_profile_end("s2_payload_write", rx, rz, span, batch, 0);
    HOOK("data_written");
    span = mc_save_profile_begin();
    if (sync_fd(fd) != 0)
        goto done;
    mc_save_profile_end("s2_data_sync", rx, rz, span, 0, 0);
    HOOK("data_synced");
    HOOK("header_begin");
    if (mc_file_pwrite_all(fd, journal_data + NEW_HEADER_OFFSET, 8192, 0) != 0)
        goto done;
    HOOK("header_written");
    span = mc_save_profile_begin();
    if (sync_fd(fd) != 0)
        goto done;
    mc_save_profile_end("s2_metadata_sync", rx, rz, span, 0, 0);
    HOOK("region_synced");
    {
        int f = fd;
        fd = -1;
        if (close(f) != 0)
            goto done;
    }
    HOOK("before_commit");
    span = mc_save_profile_begin();
    if (finish_state(s, state_name, state, journal_data, journal_size, 1) != 0)
        goto done;
    mc_save_profile_end("s2_commit_state", rx, rz, span, STATE_BYTES, 0);
    HOOK("transaction_committed");
    HOOK("before_cleanup");
    if (remove_at(s->dir, journal) != 0)
        goto done;
    HOOK("journal_unlinked");
    span = mc_save_profile_begin();
    if (sync_fd(s->dir) != 0)
        goto done;
    mc_save_profile_end("s2_commit_sync", rx, rz, span, 0, 0);
    HOOK("committed");
    rc = 0;
done:;
    int e = errno;
    if (fd >= 0 && close(fd) != 0 && rc == 0) {
        rc = -2;
        e = errno;
    }
    if (lock >= 0 && close(lock) != 0 && rc == 0) {
        rc = -2;
        e = errno;
    }
    if (rc != 0) {
        log_error("experimental Anvil save not acknowledged region=(%d,%d) journal_live=%d errno=%d", rx, rz,
                  journal_live, e);
        if (journal_live)
            rc = -2;
        if (s)
            s->failed = true;
    }
    if (p) {
        for (size_t i = 0; i < count; i++)
            free(p[i].record);
        free(p);
    }
    free(journal_data);
    free(state_data);
    mc_region_plan_free(plan);
    mc_region_index_free(index);
    errno = e;
    return rc;
}
