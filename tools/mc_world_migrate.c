/* Offline S3C tool. Never linked into mc_server. Linux, ordinary user, no
 * source writes, no deletion/cleanup of orphan stages, no backend activation. */
#define _GNU_SOURCE
#include "mc_chunk_migration.h"
#include "mc_anvil.h"
#include "mc_world_storage.h"
#include "mc_atomic_file.h"
#include <openssl/evp.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#ifdef MC_MIGRATION_TESTING
void mc_migration_test_hook(const char *phase);
#define HOOK(p) mc_migration_test_hook(p)
#else
#define HOOK(p) ((void)0)
#endif
#define MAX_FILES 100000u
#define MAX_FILE_BYTES (512u * 1024u * 1024u)
#define MANIFEST "goatcore-migration.json"
#define REGION "dimensions/minecraft/overworld/region"
enum kind { DIRECTORY, COPY, CHUNK, BINDING, LOCK, BAD };
typedef struct {
    char *path;
    struct stat st;
    enum kind kind;
    char source[65], target[65], reason[160];
    int32_t cx, cz, rx, rz;
    unsigned slot;
    size_t raw_length;
    mc_chunk_migration_info_t info;
    bool coordinates;
} entry_t;
typedef struct {
    entry_t *items;
    size_t count, cap;
} catalog_t;
static uint64_t read_bytes, written_bytes;
static unsigned errors;
static uint64_t clock_ns(clockid_t clock) {
    struct timespec t;
    if (clock_gettime(clock, &t)) {
        perror("clock_gettime");
        exit(1);
    }
    return (uint64_t)t.tv_sec * 1000000000u + t.tv_nsec;
}
static unsigned long peak_rss(void) {
    /* Linux ru_maxrss can retain a pre-exec parent's peak: use this image. */
    FILE *f = fopen("/proc/self/status", "r");
    if (!f)
        return 0;
    char line[256];
    unsigned long peak = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long n;
        if (sscanf(line, "VmHWM: %lu kB", &n) == 1)
            peak = n;
    }
    if (fclose(f))
        return 0;
    return peak;
}
static void string(FILE *out, const char *s) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\')
            fprintf(out, "\\%c", *p);
        else if (*p < 32 || *p >= 127)
            fprintf(out, "\\u%04x", *p);
        else
            fputc(*p, out);
    }
    fputc('"', out);
}
static void issue(entry_t *e, const char *why) {
    if (!*e->reason)
        snprintf(e->reason, sizeof(e->reason), "%s", why);
}
static bool same_stat(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_mode == b->st_mode &&
           a->st_nlink == b->st_nlink && a->st_size == b->st_size && a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec && a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
           a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}
static int directory(int parent, const char *name) {
    return openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
}
/* Every component is pinned and NOFOLLOW, including during rechecks/copies. */
static int relative_open(int root, const char *path, int flags) {
    char *copy = strdup(path);
    if (!copy)
        return -1;
    int fd = directory(root, ".");
    if (fd < 0) {
        free(copy);
        return -1;
    }
    char *part = copy, *slash;
    while ((slash = strchr(part, '/'))) {
        *slash = 0;
        int next = directory(fd, part), saved = errno;
        close(fd);
        fd = next;
        errno = saved;
        if (fd < 0)
            break;
        part = slash + 1;
    }
    if (fd >= 0) {
        int next = openat(fd, part, flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600), saved = errno;
        if (close(fd) && next >= 0) {
            saved = errno;
            close(next);
            next = -1;
        }
        fd = next;
        errno = saved;
    }
    free(copy);
    return fd;
}
static int close_checked(int fd, int rc) {
    int saved = errno;
    if (close(fd) && !rc) {
        rc = -1;
        saved = errno;
    }
    errno = saved;
    return rc;
}
static int sync_checked(int fd) {
    int rc;
    do {
        rc = fsync(fd);
    } while (rc < 0 && errno == EINTR);
    return rc;
}
static void digest_hex(const unsigned char bytes[32], char hex[65]) {
    for (unsigned i = 0; i < 32; i++)
        snprintf(hex + 2 * i, 3, "%02x", bytes[i]);
}
static int memory_hash(const uint8_t *data, size_t length, char out[65]) {
    unsigned char digest[32];
    unsigned n = 0;
    if (!EVP_Digest(data, length, digest, &n, EVP_sha256(), NULL) || n != 32) {
        errno = EIO;
        return -1;
    }
    digest_hex(digest, out);
    return 0;
}
static int file_hash(int fd, const struct stat *expected, char out[65]) {
    struct stat before, after;
    if (fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_nlink != 1 || before.st_size < 0 ||
        before.st_size > MAX_FILE_BYTES || (expected && !same_stat(&before, expected))) {
        errno = ESTALE;
        return -1;
    }
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    int rc = -1;
    if (!context || !EVP_DigestInit_ex(context, EVP_sha256(), NULL))
        goto done;
    uint8_t buffer[65536];
    for (off_t off = 0; off < before.st_size;) {
        size_t n = (size_t)(before.st_size - off);
        if (n > sizeof(buffer))
            n = sizeof(buffer);
        if (mc_file_pread_all(fd, buffer, n, off))
            goto done;
        read_bytes += n;
        if (!EVP_DigestUpdate(context, buffer, n))
            goto done;
        off += (off_t)n;
    }
    unsigned char digest[32];
    unsigned n = 0;
    if (fstat(fd, &after) || !same_stat(&before, &after) || !EVP_DigestFinal_ex(context, digest, &n) ||
        n != 32) {
        errno = ESTALE;
        goto done;
    }
    digest_hex(digest, out);
    rc = 0;
done:
    EVP_MD_CTX_free(context);
    return rc;
}
static int add(catalog_t *c, const char *path, const struct stat *st, entry_t **out) {
    if (c->count >= MAX_FILES) {
        errno = EFBIG;
        return -1;
    }
    if (c->count == c->cap) {
        size_t n = c->cap ? c->cap * 2 : 64;
        entry_t *p = realloc(c->items, n * sizeof(*p));
        if (!p)
            return -1;
        c->items = p;
        c->cap = n;
    }
    entry_t *e = &c->items[c->count++];
    memset(e, 0, sizeof(*e));
    e->path = strdup(path);
    if (!e->path)
        return -1;
    e->st = *st;
    e->kind = S_ISDIR(st->st_mode) ? DIRECTORY : COPY;
    *out = e;
    return 0;
}
static int walk(int fd, const char *prefix, catalog_t *c, unsigned depth) {
    if (depth > 32) {
        errno = ELOOP;
        return -1;
    }
    int own = directory(fd, ".");
    if (own < 0)
        return -1;
    DIR *d = fdopendir(own);
    if (!d) {
        close(own);
        return -1;
    }
    int rc = 0;
    for (;;) {
        errno = 0;
        struct dirent *de = readdir(d);
        if (!de) {
            if (errno)
                rc = -1;
            break;
        }
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        char name[4096];
        int n = snprintf(name, sizeof(name), "%s%s%s", prefix, *prefix ? "/" : "", de->d_name);
        struct stat st;
        entry_t *e = NULL;
        if (n < 0 || (size_t)n >= sizeof(name) || fstatat(fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) ||
            add(c, name, &st, &e)) {
            rc = -1;
            break;
        }
        for (const unsigned char *p = (const unsigned char *)name; *p; p++)
            if (*p < 32 || *p >= 127)
                issue(e, "non-ASCII/control path unsupported");
        if (S_ISDIR(st.st_mode)) {
            int next = directory(fd, de->d_name);
            if (next < 0) {
                issue(e, "directory open failed");
                continue;
            }
            if (walk(next, name, c, depth + 1))
                rc = -1;
            if (close_checked(next, 0))
                rc = -1;
            if (rc)
                break;
        } else if (!S_ISREG(st.st_mode) || st.st_nlink != 1)
            issue(e, "symlink, special file or hardlink refused");
    }
    int saved = errno;
    if (closedir(d) && !rc) {
        rc = -1;
        saved = errno;
    }
    errno = saved;
    return rc;
}
static int path_compare(const void *a, const void *b) {
    return strcmp(((const entry_t *)a)->path, ((const entry_t *)b)->path);
}
static int chunk_compare(const void *a, const void *b) {
    const entry_t *x = *(entry_t *const *)a, *y = *(entry_t *const *)b;
    if (x->rx != y->rx)
        return x->rx < y->rx ? -1 : 1;
    if (x->rz != y->rz)
        return x->rz < y->rz ? -1 : 1;
    return x->slot < y->slot ? -1 : x->slot > y->slot;
}
static void catalog_free(catalog_t *c) {
    for (size_t i = 0; i < c->count; i++)
        free(c->items[i].path);
    free(c->items);
}
static bool under(const char *path, const char *parent) {
    return !strcmp(path, parent) || (!strncmp(path, parent, strlen(parent)) && path[strlen(parent)] == '/');
}
static bool ends(const char *s, const char *suffix) {
    size_t n = strlen(s), k = strlen(suffix);
    return n >= k && !strcmp(s + n - k, suffix);
}
static bool output_alias(const char *root) {
    /* Refuse caller redirections into source/destination before emitting any
     * report. A shell may already have created/truncated that file; we cannot
     * undo the shell's action, but must not append our JSON to world data. */
    struct stat outputs[2];
    bool regular[2] = {false, false};
    for (int fd = STDOUT_FILENO; fd <= STDERR_FILENO; fd++) {
        struct stat st;
        if (fstat(fd, &st) || !S_ISREG(st.st_mode))
            continue;
        outputs[fd - 1] = st;
        regular[fd - 1] = true;
        if (st.st_nlink != 1)
            return true;
        char link[64], path[4096];
        snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(link, path, sizeof(path) - 1);
        if (n < 0 || n >= (ssize_t)sizeof(path) - 1)
            return true;
        path[n] = 0;
        if (under(path, root))
            return true;
    }
    /* A bind mount has another pathname but the same file identities. Check
     * before any diagnostic output, including refusals before locking. */
    if (!regular[0] && !regular[1])
        return false;
    struct stat st;
    if (lstat(root, &st))
        return errno != ENOENT;
    if (!S_ISDIR(st.st_mode)) {
        for (unsigned i = 0; i < 2; i++)
            if (regular[i] && outputs[i].st_dev == st.st_dev && outputs[i].st_ino == st.st_ino)
                return true;
        return false;
    }
    catalog_t files = {0};
    int fd = directory(AT_FDCWD, root);
    bool unsafe = fd < 0;
    if (fd >= 0) {
        unsafe = walk(fd, "", &files, 0) != 0;
        if (close_checked(fd, 0))
            unsafe = true;
    }
    for (size_t j = 0; j < files.count && !unsafe; j++)
        for (unsigned i = 0; i < 2; i++)
            if (regular[i] && outputs[i].st_dev == files.items[j].st.st_dev &&
                outputs[i].st_ino == files.items[j].st.st_ino)
                unsafe = true;
    catalog_free(&files);
    return unsafe;
}
static bool coordinate(const char *s, char **end, int32_t *out) {
    errno = 0;
    long long n = strtoll(s, end, 10);
    if (*end == s || errno || n < INT32_MIN || n > INT32_MAX)
        return false;
    *out = (int32_t)n;
    return true;
}
static void classify(entry_t *e) {
    if (e->kind == DIRECTORY)
        return;
    const char *p = e->path;
    if (under(p, "chunks")) {
        e->kind = CHUNK;
        char *end;
        if (strncmp(p, "chunks/c_", 9) || !coordinate(p + 9, &end, &e->cx) || *end != '_' ||
            !coordinate(end + 1, &end, &e->cz) || strcmp(end, ".bin")) {
            issue(e, "invalid/orphan legacy chunk filename");
            return;
        }
        e->coordinates = true;
        mc_region_coordinates(e->cx, e->cz, &e->rx, &e->rz, &e->slot);
        char name[100];
        snprintf(name, sizeof(name), "chunks/c_%d_%d.bin", e->cx, e->cz);
        if (strcmp(name, p))
            issue(e, "noncanonical chunk filename");
    } else if (!strcmp(p, ".goatcore-storage"))
        e->kind = BINDING;
    else if (!strcmp(p, ".goatcore-storage.lock") || !strcmp(p, "session.lock"))
        e->kind = LOCK;
    else if (under(p, "containers"))
        issue(e, "container sidecars need a separate verified inventory migration");
    else if (ends(p, ".mcc") || ends(p, ".bin") || strstr(p, ".s2-") || under(p, REGION) ||
             (under(p, "region") && ends(p, ".mca")))
        issue(e, "mixed backend, external data or recovery journal refused");
    else if (!strcmp(p, MANIFEST))
        issue(e, "reserved migration manifest in source");
}
static int read_buffer(int root, const entry_t *e, uint8_t **out, size_t *length) {
    *out = NULL;
    *length = 0;
    if (e->st.st_size <= 0 || e->st.st_size > 16u * 1024u * 1024u) {
        errno = EFBIG;
        return -1;
    }
    int fd = relative_open(root, e->path, O_RDONLY);
    if (fd < 0)
        return -1;
    struct stat st;
    int rc = -1;
    if (fstat(fd, &st) || !same_stat(&st, &e->st)) {
        errno = ESTALE;
        goto done;
    }
    uint8_t *p = malloc((size_t)st.st_size);
    if (!p)
        goto done;
    if (mc_file_pread_all(fd, p, (size_t)st.st_size, 0)) {
        free(p);
        goto done;
    }
    read_bytes += (uint64_t)st.st_size;
    if (fstat(fd, &st) || !same_stat(&st, &e->st)) {
        free(p);
        errno = ESTALE;
        goto done;
    }
    *out = p;
    *length = (size_t)st.st_size;
    rc = 0;
done:
    rc = close_checked(fd, rc);
    if (rc && *out) {
        free(*out);
        *out = NULL;
        *length = 0;
    }
    return rc;
}
static int translate(int root, entry_t *e, uint8_t **output, size_t *length) {
    HOOK("read-before");
    uint8_t *compressed = NULL, *raw = NULL;
    size_t n = 0;
    if (read_buffer(root, e, &compressed, &n)) {
        issue(e, "source chunk read failed or changed");
        return -1;
    }
    raw = malloc(MC_MIGRATION_MAX_NBT);
    if (!raw) {
        free(compressed);
        return -1;
    }
    z_stream z = {0};
    int rc = -1;
    z.next_in = compressed;
    z.avail_in = (uInt)n;
    z.next_out = raw;
    z.avail_out = MC_MIGRATION_MAX_NBT;
    if (inflateInit2(&z, 15 + 32) != Z_OK)
        goto done;
    int code = inflate(&z, Z_FINISH);
    bool valid = code == Z_STREAM_END && z.total_in == n;
    size_t raw_n = (size_t)z.total_out;
    if (inflateEnd(&z) != Z_OK)
        valid = false;
    if (!valid) {
        issue(e, "invalid/trailing compressed stream or uncompressed bound exceeded");
        errno = EINVAL;
        goto done;
    }
    HOOK("convert");
    char reason[160];
    if (mc_chunk_migrate_nbt(raw, raw_n, e->cx, e->cz, output, length, &e->info, reason)) {
        issue(e, reason);
        goto done;
    }
    e->raw_length = *length;
    if (memory_hash(*output, *length, e->target)) {
        free(*output);
        *output = NULL;
        goto done;
    }
    rc = 0;
done:
    free(raw);
    free(compressed);
    return rc;
}
static void inventory_line(const entry_t *e) {
    fputs("{\"event\":\"source\",\"path\":", stdout);
    string(stdout, e->path);
    fprintf(stdout, ",\"kind\":%d,\"bytes\":%jd,\"status\":", e->kind, (intmax_t)e->st.st_size);
    string(stdout, *e->reason ? "refused" : "accepted");
    fputs(",\"reason\":", stdout);
    string(stdout, e->reason);
    if (e->kind == CHUNK)
        fprintf(stdout, ",\"x\":%d,\"z\":%d,\"data_version\":%d,\"region_x\":%d,\"region_z\":%d", e->cx,
                e->cz, e->info.source_version, e->rx, e->rz);
    fputs(",\"source_sha256\":", stdout);
    string(stdout, e->source);
    if (e->kind == CHUNK)
        fprintf(stdout, ",\"compact_sections\":%u,\"padded_sections\":%u,\"block_entities\":%u",
                e->info.compact_sections, e->info.padded_sections, e->info.block_entities);
    fputs("}\n", stdout);
}
static int inspect(int root, catalog_t *c) {
    for (size_t i = 0; i < c->count; i++) {
        entry_t *e = &c->items[i];
        classify(e);
        if (e->kind == DIRECTORY || *e->reason)
            continue;
        int fd = relative_open(root, e->path, O_RDONLY);
        if (fd < 0) {
            issue(e, "source open failed");
            continue;
        }
        ssize_t attrs = flistxattr(fd, NULL, 0);
        if (attrs > 0 || (attrs < 0 && errno != ENOTSUP))
            issue(e, "extended attributes require separate preservation");
        if (file_hash(fd, &e->st, e->source))
            issue(e, "source hash/read failed or changed");
        if (e->kind == BINDING || e->kind == LOCK) {
            char small[64] = {0};
            if (e->st.st_size > 63 || mc_file_pread_all(fd, small, (size_t)e->st.st_size, 0))
                issue(e, "invalid backend/lock file");
            else if (e->kind == BINDING && strcmp(small, "bin\n"))
                issue(e, "source is not a BIN backend");
            else if (e->kind == LOCK && e->st.st_size &&
                     (strcmp(e->path, "session.lock") || e->st.st_size != 3 ||
                      memcmp(small, "\xe2\x98\x83", 3)))
                issue(e, "unknown lock payload");
        }
        if (close_checked(fd, 0))
            issue(e, "source close failed");
        if (e->kind == CHUNK && !*e->reason) {
            uint8_t *out = NULL;
            size_t n = 0;
            if (translate(root, e, &out, &n) && !*e->reason)
                issue(e, "chunk translation failed");
            if (!*e->reason) {
                uLongf packed = compressBound((uLong)n);
                uint8_t *compressed = malloc((size_t)packed);
                if (!compressed ||
                    compress2(compressed, &packed, out, (uLong)n, Z_DEFAULT_COMPRESSION) != Z_OK)
                    issue(e, "preflight compression failed");
                else if (packed + 5 > 255u * 4096u)
                    issue(e, "chunk requires unsupported external .mcc record");
                free(compressed);
            }
            free(out);
        } else if (e->kind == BINDING) {
            const char *native_binding = "anvil-atomic-experimental\n";
            if (memory_hash((const uint8_t *)native_binding, strlen(native_binding), e->target))
                issue(e, "binding hash failed");
        } else if (e->kind == LOCK)
            *e->target = 0; /* A lock is inventoried, never copied as world data. */
        else
            strcpy(e->target, e->source);
    }
    return 0;
}
static int source_unchanged(int root, const char *path, const struct stat *original, const catalog_t *c) {
    catalog_t now = {0};
    int rc = -1;
    struct stat pinned, named;
    if (fstat(root, &pinned) || lstat(path, &named) || !same_stat(original, &pinned) ||
        named.st_dev != pinned.st_dev || named.st_ino != pinned.st_ino || !S_ISDIR(named.st_mode)) {
        errno = ESTALE;
        goto done;
    }
    if (walk(root, "", &now, 0))
        goto done;
    qsort(now.items, now.count, sizeof(*now.items), path_compare);
    if (c->count != now.count) {
        errno = ESTALE;
        goto done;
    }
    for (size_t i = 0; i < c->count; i++) {
        const entry_t *e = &c->items[i];
        if (strcmp(e->path, now.items[i].path) || !same_stat(&e->st, &now.items[i].st)) {
            errno = ESTALE;
            goto done;
        }
        if (e->kind == DIRECTORY)
            continue;
        int fd = relative_open(root, e->path, O_RDONLY);
        if (fd < 0)
            goto done;
        char hash[65];
        int result = file_hash(fd, &e->st, hash);
        result = close_checked(fd, result);
        if (result || strcmp(hash, e->source)) {
            errno = ESTALE;
            goto done;
        }
    }
    rc = 0;
done:
    catalog_free(&now);
    return rc;
}
static char *manifest(const catalog_t *c, size_t *length) {
    char *data = NULL;
    FILE *out = open_memstream(&data, length);
    if (!out)
        return NULL;
    fputs("{\"schema\":1,\"format\":\"GoatCore BIN to Anvil "
          "26.1.1\",\"DataVersion\":4788,\"publication\":\"manifest verifies content; successful exit after "
          "parent fsync confirms publication\",\"entries\":[",
          out);
    for (size_t i = 0; i < c->count; i++) {
        const entry_t *e = &c->items[i];
        if (i)
            fputc(',', out);
        fputs("{\"path\":", out);
        string(out, e->path);
        fprintf(out, ",\"kind\":%d,\"source_size\":%jd,\"source_sha256\":", e->kind,
                (intmax_t)(e->kind == DIRECTORY ? 0 : e->st.st_size));
        string(out, e->source);
        fputs(",\"target_sha256\":", out);
        string(out, e->target);
        if (e->kind == CHUNK)
            fprintf(out, ",\"x\":%d,\"z\":%d,\"source_version\":%d,\"target_nbt_size\":%zu", e->cx, e->cz,
                    e->info.source_version, e->raw_length);
        fputc('}', out);
    }
    fputs("]}\n", out);
    int failed = ferror(out);
    if (fclose(out))
        failed = 1;
    if (failed) {
        free(data);
        return NULL;
    }
    return data;
}
static int proc_path(char out[4096], int fd, const char *relative) {
    int n = snprintf(out, 4096, "/proc/self/fd/%d/%s", fd, relative);
    if (n < 0 || n >= 4096) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}
static int make_directory(int root, const char *name) {
    /* Private unpublished tree: establish each component under pinned FDs.
     * A bottom-up sync_tree barrier durably establishes the complete tree. */
    char *copy = strdup(name), *save = NULL;
    if (!copy)
        return -1;
    int fd = directory(root, ".");
    if (fd < 0) {
        free(copy);
        return -1;
    }
    int rc = 0;
    for (char *part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        int r;
        do {
            r = mkdirat(fd, part, 0700);
        } while (r < 0 && errno == EINTR);
        if (r && errno != EEXIST) {
            rc = -1;
            break;
        }
        int next = directory(fd, part);
        if (next < 0) {
            rc = -1;
            break;
        }
        if (close_checked(fd, 0)) {
            close(next);
            fd = -1;
            rc = -1;
            break;
        }
        fd = next;
    }
    if (fd >= 0)
        rc = close_checked(fd, rc);
    free(copy);
    return rc;
}
static int copy_files(int source, int stage, const catalog_t *c) {
    for (size_t i = 0; i < c->count; i++) {
        const entry_t *e = &c->items[i];
        if (e->kind == DIRECTORY) {
            if (!under(e->path, "chunks") && make_directory(stage, e->path))
                return -1;
            continue;
        }
        if (e->kind != COPY)
            continue;
        int input = relative_open(source, e->path, O_RDONLY);
        if (input < 0)
            return -1;
        int output = relative_open(stage, e->path, O_CREAT | O_EXCL | O_WRONLY);
        if (output < 0) {
            close(input);
            return -1;
        }
        int rc = 0;
        uint8_t bytes[65536];
        for (off_t offset = 0; offset < e->st.st_size;) {
            size_t n = (size_t)(e->st.st_size - offset);
            if (n > sizeof(bytes))
                n = sizeof(bytes);
            if (mc_file_pread_all(input, bytes, n, offset) || mc_file_write_all(output, bytes, n)) {
                rc = -1;
                break;
            }
            read_bytes += n;
            written_bytes += n;
            offset += (off_t)n;
        }
        if (!rc && (fchmod(output, e->st.st_mode & 0777) || sync_checked(output)))
            rc = -1;
        rc = close_checked(output, rc);
        rc = close_checked(input, rc);
        if (rc)
            return -1;
    }
    return 0;
}
static int region_name(char path[256], const entry_t *e) {
    int n = snprintf(path, 256, REGION "/r.%d.%d.mca", e->rx, e->rz);
    return n < 0 || n >= 256 ? -1 : 0;
}
static int write_chunks(int source, int stage, entry_t **chunks, size_t count, size_t *converted) {
    for (size_t first = 0; first < count;) {
        mc_anvil_chunk_write_t batch[64];
        uint8_t *buffers[64] = {0};
        size_t amount = 0, total = 0;
        entry_t *e = chunks[first];
        char name[256], path[4096];
        int rc = -1;
        if (region_name(name, e) || proc_path(path, stage, name))
            return -1;
        while (first + amount < count && amount < 64) {
            entry_t *next = chunks[first + amount];
            if (next->rx != e->rx || next->rz != e->rz ||
                (amount && total + next->raw_length > 32u * 1024u * 1024u))
                break;
            size_t n = 0;
            char expected[65];
            strcpy(expected, next->target);
            if (translate(source, next, &buffers[amount], &n) || strcmp(expected, next->target))
                goto cleanup;
            batch[amount] = (mc_anvil_chunk_write_t){next->slot, buffers[amount], n, NULL, 0};
            total += n;
            amount++;
        }
        HOOK("region-write");
        if (mc_anvil_write_batch_expected(path, batch, amount))
            goto cleanup;
        *converted += amount;
        first += amount;
        rc = 0;
    cleanup:
        for (size_t i = 0; i < 64; i++)
            free(buffers[i]);
        if (rc)
            return -1;
    }
    return 0;
}
static bool known_generated(const entry_t *e, entry_t **chunks, size_t count) {
    const char *p = e->path;
    if (!strcmp(p, ".goatcore-storage") || !strcmp(p, MANIFEST))
        return e->kind != DIRECTORY;
    if (!strcmp(p, ".goatcore-storage.lock") || !strcmp(p, "session.lock") ||
        !strcmp(p, REGION "/.s2-owner.lock"))
        return S_ISREG(e->st.st_mode) && e->st.st_size == 0;
    if (e->kind == DIRECTORY) {
        const char *dirs[] = {
            "players", "containers", "dimensions", "dimensions/minecraft", "dimensions/minecraft/overworld",
            REGION};
        for (unsigned i = 0; i < 6; i++)
            if (!strcmp(p, dirs[i]))
                return true;
    }
    for (size_t i = 0; i < count; i++) {
        char region[256], lock[270];
        region_name(region, chunks[i]);
        snprintf(lock, sizeof(lock), "%s.lock", region);
        if (!strcmp(p, region))
            return S_ISREG(e->st.st_mode);
        if (!strcmp(p, lock))
            return S_ISREG(e->st.st_mode) && e->st.st_size == 0;
    }
    return false;
}
static int verify(int source, int dest, const catalog_t *catalog, entry_t **chunks, size_t count,
                  const char *text, size_t text_n, size_t *verified) {
    HOOK("verify");
    catalog_t files = {0};
    int rc = -1;
    if (walk(dest, "", &files, 0))
        goto done;
    for (size_t i = 0; i < files.count; i++) {
        entry_t *f = &files.items[i];
        if (*f->reason) {
            errno = EINVAL;
            goto done;
        }
        if (known_generated(f, chunks, count))
            continue;
        bool found = false;
        for (size_t j = 0; j < catalog->count; j++) {
            const entry_t *e = &catalog->items[j];
            if (!strcmp(e->path, f->path) &&
                (e->kind == COPY || (e->kind == DIRECTORY && !under(e->path, "chunks")))) {
                found = true;
                break;
            }
        }
        if (!found) {
            errno = EINVAL;
            goto done;
        }
    }
    for (size_t i = 0; i < catalog->count; i++) {
        const entry_t *e = &catalog->items[i];
        if (e->kind != COPY && e->kind != DIRECTORY)
            continue;
        if (e->kind == DIRECTORY && under(e->path, "chunks"))
            continue;
        int fd = relative_open(dest, e->path, O_RDONLY);
        if (fd < 0)
            goto done;
        struct stat st;
        int r = fstat(fd, &st);
        if (!r && e->kind == DIRECTORY && !S_ISDIR(st.st_mode))
            r = -1;
        if (!r && e->kind == COPY) {
            char hash[65];
            r = file_hash(fd, NULL, hash);
            if (!r && strcmp(hash, e->source))
                r = -1;
        }
        r = close_checked(fd, r);
        if (r)
            goto done;
    }
    int marker = relative_open(dest, ".goatcore-storage", O_RDONLY);
    if (marker < 0)
        goto done;
    char bind[64] = {0};
    struct stat st;
    int r = fstat(marker, &st);
    const char *expected = "anvil-atomic-experimental\n";
    if (!r && st.st_size == (off_t)strlen(expected))
        r = mc_file_pread_all(marker, bind, strlen(expected), 0);
    else
        r = -1;
    if (!r && strcmp(bind, expected))
        r = -1;
    r = close_checked(marker, r);
    if (r)
        goto done;
    int mf = relative_open(dest, MANIFEST, O_RDONLY);
    if (mf < 0)
        goto done;
    uint8_t *buffer = malloc(text_n ? text_n : 1);
    r = buffer ? 0 : -1;
    if (!r && (fstat(mf, &st) || st.st_size != (off_t)text_n || mc_file_pread_all(mf, buffer, text_n, 0) ||
               memcmp(buffer, text, text_n)))
        r = -1;
    free(buffer);
    r = close_checked(mf, r);
    if (r)
        goto done;
    for (size_t first = 0; first < count;) {
        entry_t *e = chunks[first];
        char name[256], path[4096];
        mc_region_file_t *file = NULL;
        if (region_name(name, e) || proc_path(path, dest, name) || mc_region_open(path, &file))
            goto done;
        size_t end = first;
        while (end < count && chunks[end]->rx == e->rx && chunks[end]->rz == e->rz)
            end++;
        r = mc_region_stats(mc_region_file_index(file))->chunks == end - first ? 0 : -1;
        for (size_t i = first; !r && i < end; i++) {
            uint8_t *actual = NULL, *want = NULL;
            size_t actual_n = 0, want_n = 0;
            if (translate(source, chunks[i], &want, &want_n) ||
                mc_anvil_read_chunk_indexed(file, chunks[i]->slot, &actual, &actual_n) ||
                actual_n != want_n || memcmp(actual, want, want_n))
                r = -1;
            free(actual);
            free(want);
            if (!r)
                (*verified)++;
        }
        if (mc_region_close(file))
            r = -1;
        if (r)
            goto done;
        first = end;
    }
    rc = 0;
done:
    catalog_free(&files);
    return rc;
}
static int sync_tree(int fd) {
    int own = directory(fd, ".");
    if (own < 0)
        return -1;
    DIR *d = fdopendir(own);
    if (!d) {
        close(own);
        return -1;
    }
    int rc = 0;
    for (;;) {
        errno = 0;
        struct dirent *e = readdir(d);
        if (!e) {
            if (errno)
                rc = -1;
            break;
        }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        struct stat st;
        if (fstatat(fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
            rc = -1;
            break;
        }
        int child = openat(fd, e->d_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (child < 0) {
            rc = -1;
            break;
        }
        int r = S_ISDIR(st.st_mode) ? sync_tree(child) : sync_checked(child);
        r = close_checked(child, r);
        if (r) {
            rc = -1;
            break;
        }
    }
    int saved = errno;
    if (closedir(d) && !rc) {
        rc = -1;
        saved = errno;
    }
    errno = saved;
    if (!rc)
        rc = sync_checked(fd);
    return rc;
}
static int lock_source(int root, const char *name, bool session) {
    int fd = relative_open(root, name, O_RDONLY);
    if (fd < 0)
        return errno == ENOENT ? -2 : -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    int r;
    /* OFD locks conflict with Vanilla's POSIX write lock, but survive closing
     * another read descriptor for the same file during hashing/sync. */
    if (session) {
        struct flock lock = {.l_type = F_RDLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0};
        r = fcntl(fd, F_OFD_SETLK, &lock);
    } else
        r = flock(fd, LOCK_EX | LOCK_NB);
    if (r) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}
static int root_open(const char *input, char real[4096]) {
    /* Refuse symlink components instead of silently choosing an alias. */
    if (!input || !*input || !realpath(input, real))
        return -1;
    int fd = directory(AT_FDCWD, *input == '/' ? "/" : ".");
    if (fd < 0)
        return -1;
    char *parts = strdup(input), *save = NULL;
    if (!parts) {
        close(fd);
        return -1;
    }
    for (char *p = strtok_r(parts, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
        int next = directory(fd, p), saved = errno;
        if (close(fd) && next >= 0) {
            saved = errno;
            close(next);
            next = -1;
        }
        fd = next;
        errno = saved;
        if (fd < 0)
            break;
    }
    free(parts);
    if (fd < 0)
        return -1;
    struct stat a, b;
    if (fstat(fd, &a) || stat(real, &b) || a.st_dev != b.st_dev || a.st_ino != b.st_ino) {
        close(fd);
        errno = ESTALE;
        return -1;
    }
    return fd;
}
int main(int argc, char **argv) {
    const char *mode = NULL, *source_arg = NULL, *destination = NULL;
    bool offline = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--analyze") || !strcmp(argv[i], "--convert") || !strcmp(argv[i], "--verify")) {
            if (mode)
                goto usage;
            mode = argv[i] + 2;
        } else if (!strcmp(argv[i], "--source") && i + 1 < argc) {
            if (source_arg)
                goto usage;
            source_arg = argv[++i];
        } else if (!strcmp(argv[i], "--destination") && i + 1 < argc) {
            if (destination)
                goto usage;
            destination = argv[++i];
        } else if (!strcmp(argv[i], "--source-offline"))
            offline = true;
        else
            goto usage;
    }
    if (!mode || !source_arg || !destination || !offline)
        goto usage;
    uint64_t start = clock_ns(CLOCK_MONOTONIC), cpu = clock_ns(CLOCK_PROCESS_CPUTIME_ID), analysis_ns = 0,
             verify_ns = 0, sync_ns = 0;
    catalog_t files = {0};
    entry_t **chunks = NULL;
    size_t count = 0, regions = 0, converted = 0, verified = 0, manifest_n = 0;
    uint64_t estimate = 0, copy_bytes = 0;
    char source_path[4096], parent_path[4096], dest_path[4096], stage_path[4096] = "", temporary[100] = "";
    char why[180] = "", *dest_copy = NULL, *manifest_text = NULL;
    int source = -1, parent = -1, stage = -1, target = -1, owner = -1, session = -1, exit_code = 1;
    bool published = false, durable = false, missing_owner = false, unsafe_output = false;
    mc_world_storage_t *store = NULL;
#define FAIL(message)                                                                                        \
    do {                                                                                                     \
        snprintf(why, sizeof(why), "%s: %s", message, strerror(errno));                                      \
        goto done;                                                                                           \
    } while (0)
    source = root_open(source_arg, source_path);
    if (source < 0)
        FAIL("source path refused");
    if (output_alias(source_path)) {
        close(source);
        return 2;
    }
    if (!strcmp(source_path, "/")) {
        errno = EINVAL;
        FAIL("filesystem root is not a world source");
    }
    if (flock(source, LOCK_EX | LOCK_NB))
        FAIL("source directory already owned");
    owner = lock_source(source, ".goatcore-storage.lock", false);
    if (owner == -1)
        FAIL("source in use or storage lock invalid");
    missing_owner = owner == -2;
    session = lock_source(source, "session.lock", true);
    if (session == -1)
        FAIL("source Vanilla session in use or invalid");
    dest_copy = strdup(destination);
    if (!dest_copy)
        FAIL("destination allocation");
    char *slash = strrchr(dest_copy, '/');
    const char *name;
    if (slash) {
        *slash = 0;
        name = slash + 1;
        parent = root_open(*dest_copy ? dest_copy : "/", parent_path);
    } else {
        name = dest_copy;
        parent = root_open(".", parent_path);
    }
    if (parent < 0 || !*name || !strcmp(name, ".") || !strcmp(name, "..")) {
        errno = EINVAL;
        FAIL("destination parent/name refused");
    }
    if (snprintf(dest_path, sizeof(dest_path), "%s%s%s", parent_path, !strcmp(parent_path, "/") ? "" : "/",
                 name) >= (int)sizeof(dest_path)) {
        errno = ENAMETOOLONG;
        FAIL("destination path");
    }
    if (output_alias(dest_path)) {
        unsafe_output = true;
        exit_code = 2;
        goto done;
    }
    if (under(dest_path, source_path) || under(source_path, dest_path)) {
        errno = EINVAL;
        FAIL("source/destination identical or nested");
    }
    struct stat dst;
    bool exists = fstatat(parent, name, &dst, AT_SYMLINK_NOFOLLOW) == 0;
    if (!exists && errno != ENOENT)
        FAIL("destination stat");
    if ((!strcmp(mode, "verify") && !exists) || (strcmp(mode, "verify") && exists)) {
        errno = EEXIST;
        FAIL("destination must be absent for analysis/conversion and present for verification");
    }
    if (exists && (!S_ISDIR(dst.st_mode) || S_ISLNK(dst.st_mode))) {
        errno = EINVAL;
        FAIL("destination type");
    }
    struct stat src;
    if (fstat(source, &src))
        FAIL("source identity");
    if (exists && src.st_dev == dst.st_dev && src.st_ino == dst.st_ino) {
        errno = EINVAL;
        FAIL("aliased source/destination");
    }
    HOOK("analysis");
    if (walk(source, "", &files, 0))
        FAIL("source scan");
    struct stat parent_identity;
    if (fstat(parent, &parent_identity))
        FAIL("destination parent identity");
    bool parent_in_source = parent_identity.st_dev == src.st_dev && parent_identity.st_ino == src.st_ino;
    for (size_t i = 0; i < files.count && !parent_in_source; i++)
        parent_in_source = files.items[i].kind == DIRECTORY &&
                           parent_identity.st_dev == files.items[i].st.st_dev &&
                           parent_identity.st_ino == files.items[i].st.st_ino;
    if (parent_in_source) {
        errno = EINVAL;
        FAIL("destination parent aliases source directory");
    }
    qsort(files.items, files.count, sizeof(*files.items), path_compare);
    inspect(source, &files);
    chunks = calloc(files.count ? files.count : 1, sizeof(*chunks));
    if (!chunks)
        FAIL("chunk inventory");
    bool chunk_directory = false;
    for (size_t i = 0; i < files.count; i++) {
        entry_t *e = &files.items[i];
        if (e->kind == DIRECTORY && !strcmp(e->path, "chunks"))
            chunk_directory = true;
        if (e->kind == CHUNK && e->coordinates)
            chunks[count++] = e;
        if (e->kind == COPY && e->st.st_size > 0)
            copy_bytes += (uint64_t)e->st.st_size;
    }
    qsort(chunks, count, sizeof(*chunks), chunk_compare);
    for (size_t i = 0; i < count; i++) {
        entry_t *e = chunks[i];
        if (i && !chunk_compare(&chunks[i - 1], &chunks[i])) {
            issue(chunks[i - 1], "duplicate chunk coordinates");
            issue(e, "duplicate chunk coordinates");
        }
        if (!i || e->rx != chunks[i - 1]->rx || e->rz != chunks[i - 1]->rz)
            regions++;
        estimate += ((compressBound((uLong)e->raw_length) + 5 + 4095) / 4096) * 4096;
    }
    estimate += copy_bytes + regions * 8192;
    for (size_t i = 0; i < files.count; i++) {
        inventory_line(&files.items[i]);
        if (*files.items[i].reason)
            errors++;
    }
    if (!chunk_directory) {
        errors++;
        snprintf(why, sizeof(why), "source chunks directory is missing");
    }
    analysis_ns = clock_ns(CLOCK_MONOTONIC) - start;
    if (errors)
        goto done;
    if (source_unchanged(source, source_path, &src, &files))
        FAIL("source changed after analysis");
    if (!strcmp(mode, "analyze")) {
        exit_code = 0;
        goto done;
    }
    manifest_text = manifest(&files, &manifest_n);
    if (!manifest_text)
        FAIL("manifest allocation");
    if (!strcmp(mode, "verify")) {
        target = directory(parent, name);
        if (target < 0)
            FAIL("verification destination open");
        int guard = lock_source(target, ".goatcore-storage.lock", false);
        if (guard < 0)
            FAIL("verification destination in use or missing lock");
        int target_session = lock_source(target, "session.lock", true);
        if (target_session < 0) {
            close(guard);
            FAIL("verification Vanilla session in use or missing");
        }
        uint64_t t = clock_ns(CLOCK_MONOTONIC);
        int r = verify(source, target, &files, chunks, count, manifest_text, manifest_n, &verified);
        verify_ns = clock_ns(CLOCK_MONOTONIC) - t;
        r = close_checked(target_session, r);
        r = close_checked(guard, r);
        if (r)
            FAIL("destination verification");
        if (source_unchanged(source, source_path, &src, &files))
            FAIL("source changed during verification");
        exit_code = 0;
        goto done;
    }
    struct statvfs space;
    if (fstatvfs(parent, &space))
        FAIL("free-space query");
    if ((uint64_t)space.f_bavail * space.f_frsize < estimate * 2 + 64u * 1024u * 1024u) {
        errno = ENOSPC;
        FAIL("insufficient estimated staging space");
    }
    HOOK("prepare");
    unsigned char random[16];
    size_t got = 0;
    while (got < sizeof(random)) {
        ssize_t n = getrandom(random + got, sizeof(random) - got, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            FAIL("random staging name");
        got += (size_t)n;
    }
    char hex[33];
    for (unsigned i = 0; i < 16; i++)
        snprintf(hex + 2 * i, 3, "%02x", random[i]);
    snprintf(temporary, sizeof(temporary), ".goatcore-migrate.tmp.%ld.%s", (long)getpid(), hex);
    if (mkdirat(parent, temporary, 0700))
        FAIL("staging mkdir");
    stage = directory(parent, temporary);
    if (stage < 0)
        FAIL("staging open");
    if (snprintf(stage_path, sizeof(stage_path), "%s/%s", parent_path, temporary) >=
        (int)sizeof(stage_path)) {
        errno = ENAMETOOLONG;
        FAIL("staging path");
    }
    if (mc_storage_open(stage_path, MC_STORAGE_ANVIL_ATOMIC, &store))
        FAIL("native staging initialization");
    if (copy_files(source, stage, &files))
        FAIL("ancillary copy");
    if (write_chunks(source, stage, chunks, count, &converted))
        FAIL("region conversion/write");
    int mf = relative_open(stage, MANIFEST, O_CREAT | O_EXCL | O_WRONLY);
    if (mf < 0)
        FAIL("manifest create");
    int r = mc_file_write_all(mf, manifest_text, manifest_n);
    written_bytes += manifest_n;
    if (!r)
        r = sync_checked(mf);
    r = close_checked(mf, r);
    if (r)
        FAIL("manifest write/sync");
    uint64_t t = clock_ns(CLOCK_MONOTONIC);
    if (verify(source, stage, &files, chunks, count, manifest_text, manifest_n, &verified))
        FAIL("staging content verification");
    verify_ns = clock_ns(CLOCK_MONOTONIC) - t;
    if (converted != count || verified != count) {
        errno = EIO;
        FAIL("incomplete conversion counts");
    }
    if (source_unchanged(source, source_path, &src, &files))
        FAIL("source changed before publication");
    HOOK("files-sync-before");
    t = clock_ns(CLOCK_MONOTONIC);
    if (sync_tree(stage))
        FAIL("staging tree fsync");
    sync_ns = clock_ns(CLOCK_MONOTONIC) - t;
    HOOK("files-sync-after");
    HOOK("publish-before");
    if (source_unchanged(source, source_path, &src, &files))
        FAIL("source changed at publication barrier");
    if (renameat2(parent, temporary, parent, name, RENAME_NOREPLACE)) {
        int error = errno;
        struct stat visible, staged;
        if (!fstat(stage, &staged) && !fstatat(parent, name, &visible, AT_SYMLINK_NOFOLLOW) &&
            staged.st_dev == visible.st_dev && staged.st_ino == visible.st_ino) {
            published = true;
            exit_code = 3;
            errno = error;
            FAIL("rename reported failure after visible publication; durability uncertain");
        }
        errno = error;
        FAIL("non-replacing directory publication");
    }
    published = true;
    HOOK("publish-after");
    if (sync_checked(parent)) {
        exit_code = 3;
        FAIL("published image visible; durability uncertain (parent fsync)");
    }
    durable = true;
    HOOK("durable");
    exit_code = 0;
done:;
    int saved = errno;
    mc_storage_close(store);
    int fds[] = {target, stage, session, owner, source, parent};
    for (unsigned i = 0; i < sizeof(fds) / sizeof(*fds); i++)
        if (fds[i] >= 0 && close(fds[i]) && !exit_code) {
            exit_code = published ? 3 : 1;
            durable = false;
            snprintf(why, sizeof(why), "close failure; publication acknowledgement withheld");
        }
    if (unsafe_output) {
        free(chunks);
        catalog_free(&files);
        free(dest_copy);
        free(manifest_text);
        return exit_code;
    }
    uint64_t finish = clock_ns(CLOCK_MONOTONIC);
    size_t regular = 0, accepted = 0, candidates = 0, v4785 = 0, v4788 = 0, other = 0;
    for (size_t i = 0; i < files.count; i++) {
        if (files.items[i].kind != DIRECTORY)
            regular++;
        if (files.items[i].kind == CHUNK && !*files.items[i].reason)
            accepted++;
        if (files.items[i].kind == CHUNK) {
            candidates++;
            int version = files.items[i].info.source_version;
            if (version == 4785)
                v4785++;
            else if (version == 4788)
                v4788++;
            else if (version)
                other++;
        }
    }
    if (exit_code && !errors)
        errors = 1;
    fprintf(stdout, "{\"event\":\"summary\",\"mode\":");
    string(stdout, mode);
    fprintf(stdout,
            ",\"status\":\"%s\",\"complete\":%s,\"source_files\":%zu,\"chunks_found\":%zu,\"accepted\":%zu,"
            "\"converted\":%zu,\"verified\":%zu,\"regions\":%zu,\"errors\":%u,\"published\":%s,\"durable_"
            "publication\":%s,\"source_owner_lock_missing\":%s,\"estimated_output_bytes\":%" PRIu64
            ",\"source_read_bytes\":%" PRIu64 ",\"copy_and_manifest_write_bytes\":%" PRIu64
            ",\"elapsed_ms\":%.3f,\"cpu_ms\":%.3f,\"analysis_ms\":%.3f,\"verify_ms\":%.3f,\"tree_sync_ms\":%."
            "3f,\"rss_max_kb\":%lu,\"reason\":",
            exit_code ? "failed" : "passed", exit_code ? "false" : "true", regular, count, accepted,
            converted, verified, regions, errors, published ? "true" : "false", durable ? "true" : "false",
            missing_owner ? "true" : "false", estimate, read_bytes, written_bytes, (finish - start) / 1e6,
            (clock_ns(CLOCK_PROCESS_CPUTIME_ID) - cpu) / 1e6, analysis_ns / 1e6, verify_ns / 1e6,
            sync_ns / 1e6, peak_rss());
    string(stdout, why);
    fprintf(stdout, ",\"chunk_files\":%zu,\"versions\":{\"4785\":%zu,\"4788\":%zu,\"other\":%zu}", candidates,
            v4785, v4788, other);
    fputs(",\"orphan_stage\":", stdout);
    string(stdout, published ? "" : stage_path);
    fputs("}\n", stdout);
    bool output_error = ferror(stdout) || fflush(stdout) != 0;
    free(chunks);
    catalog_free(&files);
    free(dest_copy);
    free(manifest_text);
    errno = saved;
    return output_error && !exit_code ? 4 : exit_code;
usage:
    fprintf(stderr,
            "usage: %s --analyze|--convert|--verify --source WORLD --destination ABSENT_OR_VERIFIED_WORLD "
            "--source-offline\n",
            argv[0]);
    return 2;
#undef FAIL
}
