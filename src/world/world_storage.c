#include "mc_world_storage.h"
#include "mc_chunk_store.h"
#include "mc_anvil.h"
#include "mc_atomic_file.h"
#include "mc_util.h"
#include "goatworld_store.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

struct mc_world_storage {
    const char *path;
    mc_storage_kind_t kind;
    int owner, session, s2;
    mc_goatworld_t *sql;
};
bool mc_storage_is_native(const mc_world_storage_t *s) {
    return s && s->kind != MC_STORAGE_BIN;
}
bool mc_storage_is_sqlite(const mc_world_storage_t *s) { return s && s->kind==MC_STORAGE_GOATWORLD_SQLITE; }
const char *mc_storage_name(mc_storage_kind_t kind) {
    return kind == MC_STORAGE_BIN            ? "bin"
           : kind == MC_STORAGE_ANVIL_ATOMIC ? "anvil-atomic-experimental"
           : kind == MC_STORAGE_GOATWORLD_SQLITE ? "goatworld-sqlite-experimental"
                                             : "invalid";
}
int mc_storage_parse(const char *name, mc_storage_kind_t *kind) {
    if (!name || !kind) {
        errno = EINVAL;
        return -1;
    }
    if (!strcmp(name, "bin"))
        *kind = MC_STORAGE_BIN;
    else if (!strcmp(name, "anvil-atomic-experimental"))
        *kind = MC_STORAGE_ANVIL_ATOMIC;
    else if(!strcmp(name,"goatworld-sqlite-experimental"))
        *kind=MC_STORAGE_GOATWORLD_SQLITE;
    else {
        errno = EINVAL;
        return -1;
    }
    return 0;
}
static int filename(char out[4096], const char *root, const char *suffix) {
    int n = snprintf(out, 4096, "%s/%s", root, suffix);
    if (n < 0 || n >= 4096) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}
static bool ends(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !strcmp(s + a - b, suffix);
}
static int contains(const char *path, const char *suffix) {
    DIR *d = opendir(path);
    if (!d)
        return errno == ENOENT ? 0 : -1;
    int rc = 0;
    for (;;) {
        errno = 0;
        struct dirent *e = readdir(d);
        if (!e) {
            if (errno)
                rc = -1;
            break;
        }
        if (ends(e->d_name, suffix)) {
            rc = 1;
            break;
        }
    }
    int saved = errno;
    if (closedir(d) != 0 && rc >= 0) {
        rc = -1;
        saved = errno;
    }
    errno = saved;
    return rc;
}
static int lock_file(const char *path, int operation) {
    int fd;
    do {
        fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
        int e = errno ? errno : EINVAL;
        close(fd);
        errno = e;
        return -1;
    }
    int r;
    do {
        r = flock(fd, operation | LOCK_NB);
    } while (r < 0 && errno == EINTR);
    if (r != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}
int mc_storage_close_checked(mc_world_storage_t *s) {
    if(!s)return 0;
    int rc=0;
    if(mc_goatworld_close(s->sql))rc=-1;
    if(s->session>=0&&close(s->session))rc=-1;
    if(s->s2>=0&&close(s->s2))rc=-1;
    if(s->owner>=0&&close(s->owner))rc=-1;
    if(rc)log_error("storage close/checkpoint FAILED mode=%s errno=%d",mc_storage_name(s->kind),errno);
    free(s);return rc;
}
void mc_storage_close(mc_world_storage_t *s) { (void)mc_storage_close_checked(s); }
int mc_storage_open(const char *root, mc_storage_kind_t kind, mc_world_storage_t **out) {
    if (!out || (kind != MC_STORAGE_BIN && kind != MC_STORAGE_ANVIL_ATOMIC && kind != MC_STORAGE_GOATWORLD_SQLITE)) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    mc_world_storage_t *s = calloc(1, sizeof(*s));
    if (!s)
        return -1;
    s->owner = s->session = s->s2 = -1;
    s->path = root;
    s->kind = kind;
    if (!root || !*root) {
        if (kind != MC_STORAGE_BIN) {
            errno = EINVAL;
            goto bad;
        }
        *out = s;
        return 0;
    }
    if (mc_persistence_mkdirs(root, 0755) != 0 || access(root, R_OK | W_OK | X_OK) != 0)
        goto bad;
    char path[4096], binding[4096], region[4096];
    if (filename(path, root, ".goatcore-storage.lock") != 0)
        goto bad;
    s->owner = lock_file(path, kind == MC_STORAGE_BIN ? LOCK_SH : LOCK_EX);
    if (s->owner < 0)
        goto bad;
    if (filename(binding, root, ".goatcore-storage") != 0)
        goto bad;
    int fd;
    do {
        fd = open(binding, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    } while (fd < 0 && errno == EINTR);
    bool bound = fd >= 0;
    if (bound) {
        char text[64] = {0};
        struct stat st;
        int rc;
        do {
            rc = fstat(fd, &st);
        } while (rc < 0 && errno == EINTR);
        size_t expected = strlen(mc_storage_name(kind)) + 1;
        if (rc == 0 && (!S_ISREG(st.st_mode) || st.st_size != (off_t)expected)) {
            errno = EINVAL;
            rc = -1;
        }
        if (rc == 0)
            rc = mc_file_pread_all(fd, text, expected, 0);
        int saved = errno;
        if (close(fd) != 0 && rc == 0) {
            rc = -1;
            saved = errno;
        }
        errno = saved;
        if (rc != 0)
            goto bad;
        if (memcmp(text, mc_storage_name(kind), expected - 1) || text[expected - 1] != '\n') {
            errno = EINVAL;
            goto bad;
        }
    } else if (errno != ENOENT)
        goto bad;
    if (filename(region, root, "dimensions/minecraft/overworld/region") != 0)
        goto bad;
    if (kind == MC_STORAGE_BIN) {
        int r = contains(region, ".mca");
        if (r != 0) {
            if (r > 0)
                errno = EINVAL;
            goto bad;
        }
    } else {
        if (filename(path, root, "chunks") != 0)
            goto bad;
        int r = contains(path, ".bin");
        if (r != 0) {
            if (r > 0)
                errno = EINVAL;
            goto bad;
        }
        if (filename(path, root, "region") != 0)
            goto bad;
        r = contains(path, ".mca");
        if (r != 0) {
            if (r > 0)
                errno = EINVAL;
            goto bad;
        }
    }
    if(kind==MC_STORAGE_GOATWORLD_SQLITE){
        int present=contains(region,".mca");
        if(present){if(present>0)errno=EINVAL;goto bad;}
    }else{
        struct stat existing;
        if(filename(path,root,"goatworld")!=0)goto bad;
        if(lstat(path,&existing)==0){errno=EINVAL;goto bad;}
        if(errno!=ENOENT)goto bad;
    }
    const char *common[] = {"players", "containers"};
    for (unsigned i = 0; i < 2; i++) {
        if (filename(path, root, common[i]) != 0 || mc_persistence_mkdirs(path, 0755) != 0 ||
            access(path, R_OK | W_OK | X_OK) != 0)
            goto bad;
    }
    if (kind == MC_STORAGE_BIN) {
        const char *dirs[] = {"region", "chunks"};
        for (unsigned i = 0; i < 2; i++)
            if (filename(path, root, dirs[i]) != 0 || mc_persistence_mkdirs(path, 0755) != 0 ||
                access(path, R_OK | W_OK | X_OK) != 0)
                goto bad;
    } else if(kind==MC_STORAGE_ANVIL_ATOMIC) {
        if (mc_persistence_mkdirs(region, 0755) != 0 || access(region, R_OK | W_OK | X_OK) != 0)
            goto bad;
        if (filename(path, region, ".s2-owner.lock") != 0)
            goto bad;
        s->s2 = lock_file(path, LOCK_SH);
        if (s->s2 < 0)
            goto bad;
        int r = contains(region, ".s2-journal");
        if (r != 0) {
            if (r > 0)
                errno = EBUSY;
            goto bad;
        }
    } else if(mc_goatworld_open(root,bound,&s->sql))goto bad;
    if(kind!=MC_STORAGE_BIN){
        if (filename(path, root, "session.lock") != 0)
            goto bad;
        do {
            s->session = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        } while (s->session < 0 && errno == EINTR);
        if (s->session < 0)
            goto bad;
        struct stat st;
        int sr;
        do {
            sr = fstat(s->session, &st);
        } while (sr < 0 && errno == EINTR);
        if (sr != 0)
            goto bad;
        if (!S_ISREG(st.st_mode) || st.st_nlink != 1) {
            errno = EINVAL;
            goto bad;
        }
        struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0};
        int rc;
        do {
            rc = fcntl(s->session, F_SETLK, &lock);
        } while (rc < 0 && errno == EINTR);
        if (rc < 0)
            goto bad;
        if (!bound) {
            char text[64];
            int n = snprintf(text, sizeof(text), "%s\n", mc_storage_name(kind));
            mc_atomic_file_t f = {.dir_fd = -1, .temp_fd = -1, .path = binding};
            if (mc_atomic_file_begin(&f, binding) != 0)
                goto bad;
            int wr = mc_file_write_all(f.temp_fd, text, (size_t)n);
            if (!wr)
                wr = mc_atomic_file_commit(&f);
            int e = errno;
            mc_atomic_file_abort(&f);
            errno = e;
            if (wr != 0)
                goto bad;
        }
    }
    *out = s;
    return 0;
bad:;
    int e = errno;
    log_error("storage open refused mode=%s world=%s errno=%d (%s)", mc_storage_name(kind),
              root ? root : "(memory)", e, strerror(e));
    mc_storage_close(s);
    errno = e;
    return -1;
}
static int region_path(const mc_world_storage_t *s, int32_t cx, int32_t cz, char path[4096], unsigned *slot) {
    int32_t rx, rz;
    mc_region_coordinates(cx, cz, &rx, &rz, slot);
    int n = snprintf(path, 4096, "%s/dimensions/minecraft/overworld/region/r.%d.%d.mca", s->path, rx, rz);
    if (n < 0 || n >= 4096) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}
int mc_storage_read(const mc_world_storage_t *s, int32_t cx, int32_t cz, mc_chunk_t *out,
                    mc_block_entity_store_t *entities) {
    if (!s || !out) {
        errno = EINVAL;
        return -1;
    }
    mc_save_clock_t span = mc_save_profile_begin();
    int rc = -1;
    if (s->kind == MC_STORAGE_BIN)
        rc = mc_chunk_store_read(s->path, cx, cz, out, entities);
    else if(s->kind==MC_STORAGE_GOATWORLD_SQLITE)rc=mc_goatworld_read(s->sql,cx,cz,out);
    else {
        char path[4096];
        unsigned slot;
        if (region_path(s, cx, cz, path, &slot) != 0)
            return -1;
        mc_region_file_t *file = NULL;
        rc = mc_region_open(path, &file);
        if (rc == 0) {
            const mc_region_stats_t *st = mc_region_stats(mc_region_file_index(file));
            if (st->unsupported || st->external) {
                errno = EOPNOTSUPP;
                rc = -1;
            } else {
                uint8_t *raw = NULL;
                size_t n = 0;
                rc = mc_anvil_read_chunk_indexed(file, slot, &raw, &n);
                if (rc == 0) {
                    rc = mc_anvil_decode_native(raw, n, cx, cz, out);
                    if (rc == 0) {
                        out->storage_nbt = raw;
                        out->storage_nbt_len = n;
                        raw = NULL;
                    }
                }
                free(raw);
            }
            int e = errno;
            if (mc_region_close(file) != 0 && rc >= 0) {
                if (rc == 0)
                    mc_chunk_destroy(out);
                rc = -1;
                e = errno;
            }
            errno = e;
        }
    }
    /* Loading constructs palettes through setters, but is not a new gameplay
     * mutation. Begin a runtime epoch at the visible source. This is not an
     * acknowledgement of any previous process's uncertain disk operation. */
    if (rc==0 && s->kind==MC_STORAGE_ANVIL_ATOMIC) {
        out->save_generation=0;
        out->durable_generation=0;
    }
    mc_save_profile_end("storage_load", cx, cz, span, 0, rc);
    return rc;
}
int mc_storage_write(const mc_world_storage_t *s, mc_chunk_t *chunk,
                     const mc_block_entity_store_t *entities) {
    if (!s || !chunk) {
        errno = EINVAL;
        return -1;
    }
    if (s->kind == MC_STORAGE_BIN)
        return mc_chunk_store_write(s->path, chunk, entities);
    if(s->kind==MC_STORAGE_GOATWORLD_SQLITE){mc_chunk_t *one[]={chunk};return mc_goatworld_write(s->sql,one,1);}
    mc_save_clock_t span = mc_save_profile_begin();
    uint8_t *raw = NULL;
    size_t n = 0;
    char path[4096];
    unsigned slot;
    if (region_path(s, chunk->cx, chunk->cz, path, &slot) != 0)
        return -1;
    int rc = mc_anvil_encode_native(chunk, entities, &raw, &n);
    if (rc == 0)
        rc = mc_anvil_write_chunk_nbt_expected(path, slot % 32, slot / 32, raw, n, chunk->storage_nbt,
                                               chunk->storage_nbt_len);
    if (rc == 0 || rc == MC_SAVE_DURABILITY_UNCERTAIN) {
        free(chunk->storage_nbt);
        chunk->storage_nbt = raw;
        chunk->storage_nbt_len = n;
        raw = NULL;
    }
    if(rc==0)chunk->durable_generation=chunk->save_generation;
    int saved = errno;
    free(raw);
    errno = saved;
    mc_save_profile_end("native_chunk_total", chunk->cx, chunk->cz, span, n, rc);
    return rc;
}

int mc_storage_write_region(const mc_world_storage_t *s, mc_chunk_t *const *chunks, size_t count) {
    if (!mc_storage_is_native(s) || !chunks || !count || count > MC_REGION_SLOTS) {
        errno = EINVAL;
        return -1;
    }
    if(s->kind==MC_STORAGE_GOATWORLD_SQLITE)return mc_goatworld_write(s->sql,chunks,count);
    mc_save_clock_t span = mc_save_profile_begin();
    mc_anvil_chunk_write_t *writes = calloc(count, sizeof(*writes));
    if (!writes) return -1;
    char path[4096], candidate[4096];
    int rc = -1;
    size_t total = 0;
    bool seen[MC_REGION_SLOTS] = {0};
    for (size_t i = 0; i < count; i++) {
        unsigned slot;
        if (!chunks[i] || region_path(s,chunks[i]->cx,chunks[i]->cz,candidate,&slot) != 0) goto done;
        if (seen[slot] || (i && strcmp(path,candidate))) { errno = EINVAL; goto done; }
        seen[slot] = true;
        if (!i) memcpy(path,candidate,strlen(candidate)+1);
        uint8_t *raw = NULL; size_t n = 0;
        mc_save_clock_t encode = mc_save_profile_begin();
        if (mc_anvil_encode_native(chunks[i],NULL,&raw,&n) != 0) goto done;
        mc_save_profile_end("native_encode",chunks[i]->cx,chunks[i]->cz,encode,n,0);
        writes[i] = (mc_anvil_chunk_write_t){slot,raw,n,chunks[i]->storage_nbt,chunks[i]->storage_nbt_len};
        if (n > 64u*1024u*1024u - total) { errno = EFBIG; goto done; }
        total += n;
    }
    rc = mc_anvil_write_batch_expected(path,writes,count);
    if (rc == 0 || rc == MC_SAVE_DURABILITY_UNCERTAIN) {
        for (size_t i = 0; i < count; i++) {
            free(chunks[i]->storage_nbt);
            chunks[i]->storage_nbt = (uint8_t *)writes[i].nbt;
            chunks[i]->storage_nbt_len = writes[i].length;
            if(rc==0)chunks[i]->durable_generation=chunks[i]->save_generation;
            writes[i].nbt = NULL;
        }
    }
done:;
    int saved = errno;
    for (size_t i = 0; i < count; i++) free((void *)writes[i].nbt);
    free(writes);
    errno = saved;
    mc_save_profile_end("native_batch_total",chunks[0] ? chunks[0]->cx : 0,chunks[0] ? chunks[0]->cz : 0,span,total,rc);
    return rc;
}
