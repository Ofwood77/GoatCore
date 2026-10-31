/* Syscall wrapping belongs only to this test executable, never the server. */
#include "mc_atomic_file.h"
#include "mc_anvil.h"
#include "mc_chunk_store.h"
#include "mc_block_state.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>

enum operation {
    NONE,
    CREATE_TEMP,
    WRITE_TEMP,
    PWRITE_TEMP,
    READ_REGION,
    SYNC_FILE,
    RENAME_FILE,
    SYNC_DIR,
    CLOSE_FILE,
    CLOSE_DIR,
    STAT_REGION,
    LOCK_REGION,
    ALLOCATE
};
enum interruption {
    RUN,
    BEFORE_TEMP,
    PARTIAL_TEMP,
    BEFORE_SYNC,
    BEFORE_RENAME,
    AFTER_RENAME,
    AFTER_DIR_SYNC
};
static _Thread_local enum operation fault;
static _Thread_local enum interruption interruption;
static _Thread_local int injected_errno, remaining, allocation_skip;
static _Thread_local int temporary = -1, directory = -1, is_second;
static _Thread_local int replaced;
static atomic_bool block_first, second_done, fail_load_once;
static char load_failure_path[4096];
static sem_t first_preparing, release_first, second_waiting;

size_t __real_fread(void *, size_t, size_t, FILE *);
size_t __real___fread_chk(void *, size_t, size_t, size_t, FILE *);
int __real_openat(int, const char *, int, ...);
ssize_t __real_write(int, const void *, size_t);
ssize_t __real_pwrite(int, const void *, size_t, off_t);
ssize_t __real_pread(int, void *, size_t, off_t);
int __real_fsync(int);
int __real_renameat(int, const char *, int, const char *);
int __real_close(int);
int __real_fstat(int, struct stat *);
int __real_flock(int, int);
void *__real_calloc(size_t, size_t);

static int fails(enum operation op) {
    if (fault != op || !remaining)
        return 0;
    if (op == ALLOCATE && allocation_skip-- > 0)
        return 0;
    remaining--;
    errno = injected_errno;
    return 1;
}
static void crash(enum interruption point) {
    if (interruption == point) {
        kill(getpid(), SIGKILL);
        _exit(99);
    }
}
static int fail_read_stream(FILE *stream) {
    if (atomic_load(&fail_load_once)) {
        char proc[80], name[4096];
        snprintf(proc,sizeof(proc),"/proc/self/fd/%d",fileno(stream));
        ssize_t n=readlink(proc,name,sizeof(name)-1);
        if (n>=0) {
            name[n]=0;
            if (!strcmp(name,load_failure_path) && atomic_exchange(&fail_load_once,false)) { errno=EIO; return 1; }
        }
    }
    return 0;
}
size_t __wrap_fread(void *ptr, size_t size, size_t count, FILE *stream) {
    if (fail_read_stream(stream)) return 0;
    return __real_fread(ptr,size,count,stream);
}
size_t __wrap___fread_chk(void *ptr, size_t capacity, size_t size, size_t count, FILE *stream) {
    if (fail_read_stream(stream)) return 0;
    return __real___fread_chk(ptr,capacity,size,count,stream);
}

int __wrap_openat(int fd, const char *name, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    int temp = (flags & O_EXCL) && strstr(name, ".tmp.");
    if (temp) {
        crash(BEFORE_TEMP);
        if (fails(CREATE_TEMP))
            return -1;
    }
    int out = __real_openat(fd, name, flags, mode);
    if (temp && out >= 0) {
        temporary = out;
        directory = fd;
        replaced = 0;
    }
    return out;
}
static void barrier(void) {
    if (atomic_exchange(&block_first, false)) {
        assert(sem_post(&first_preparing) == 0);
        while (sem_wait(&release_first) != 0)
            assert(errno == EINTR);
    }
}
ssize_t __wrap_write(int fd, const void *data, size_t size) {
    if (fd == temporary) {
        barrier();
        if (interruption == PARTIAL_TEMP) {
            assert(__real_write(fd, data, size > 1 ? size / 2 : 1) > 0);
            crash(PARTIAL_TEMP);
        }
        if (fails(WRITE_TEMP)) {
            if (injected_errno == 0)
                return 0;
            if (injected_errno == -1)
                return __real_write(fd, data, size > 1 ? size / 2 : 1);
            return -1;
        }
    }
    return __real_write(fd, data, size);
}
ssize_t __wrap_pwrite(int fd, const void *data, size_t size, off_t offset) {
    if (fd == temporary && fails(PWRITE_TEMP)) {
        if (!injected_errno)
            return 0;
        if (injected_errno == -1)
            return __real_pwrite(fd, data, size > 1 ? size / 2 : 1, offset);
        return -1;
    }
    return __real_pwrite(fd, data, size, offset);
}
ssize_t __wrap_pread(int fd, void *data, size_t size, off_t offset) {
    if (fails(READ_REGION)) {
        if (!injected_errno)
            return 0;
        if (injected_errno == -1)
            return __real_pread(fd, data, size > 1 ? size / 2 : 1, offset);
        return -1;
    }
    return __real_pread(fd, data, size, offset);
}
int __wrap_fsync(int fd) {
    if (fd == temporary) {
        crash(BEFORE_SYNC);
        if (fails(SYNC_FILE))
            return -1;
    }
    if (fd == directory && replaced) {
        crash(AFTER_RENAME);
        if (fails(SYNC_DIR))
            return -1;
    }
    int rc = __real_fsync(fd);
    if (!rc && fd == directory && replaced)
        crash(AFTER_DIR_SYNC);
    return rc;
}
int __wrap_renameat(int a, const char *b, int c, const char *d) {
    crash(BEFORE_RENAME);
    if (fails(RENAME_FILE))
        return -1;
    int rc = __real_renameat(a, b, c, d);
    if (!rc)
        replaced = 1;
    return rc;
}
int __wrap_close(int fd) {
    int bad = (fd == temporary && fails(CLOSE_FILE)) || (fd == directory && replaced && fails(CLOSE_DIR));
    int saved = errno, rc = __real_close(fd);
    if (fd == temporary)
        temporary = -1;
    if (fd == directory)
        directory = -1;
    if (bad) {
        errno = saved;
        return -1;
    } /* Linux close(EINTR) also consumes fd. */
    return rc;
}
int __wrap_fstat(int fd, struct stat *st) {
    if (fails(STAT_REGION))
        return -1;
    return __real_fstat(fd, st);
}
int __wrap_flock(int fd, int op) {
    if (is_second)
        assert(sem_post(&second_waiting) == 0);
    if (fails(LOCK_REGION))
        return -1;
    return __real_flock(fd, op);
}
void *__wrap_calloc(size_t n, size_t size) {
    if (fails(ALLOCATE))
        return NULL;
    return __real_calloc(n, size);
}

typedef struct {
    uint8_t *data;
    size_t size;
} blob_t;
static blob_t read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(!fseek(file, 0, SEEK_END));
    long size = ftell(file);
    assert(size >= 0);
    rewind(file);
    blob_t out = {malloc((size_t)size + 1), (size_t)size};
    assert(out.data);
    assert(fread(out.data, 1, out.size, file) == out.size);
    assert(!fclose(file));
    return out;
}
static int equal(blob_t a, blob_t b) {
    return a.size == b.size && !memcmp(a.data, b.data, a.size);
}
static void restore(const char *path, blob_t data) {
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0640);
    assert(fd >= 0);
    assert(!mc_file_write_all(fd, data.data, data.size));
    assert(!fsync(fd));
    assert(!close(fd));
}
static int count_temporaries(const char *path) {
    DIR *dir = opendir(path);
    assert(dir);
    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)))
        if (strstr(entry->d_name, ".tmp."))
            count++;
    assert(!closedir(dir));
    return count;
}
static void no_temporaries(const char *path) {
    assert(!count_temporaries(path));
}

static mc_global_state_id_t stone, stair;
static mc_block_entity_store_t entities;
static blob_t make_raw(int cx, int cz, mc_global_state_id_t state, int extra) {
    mc_chunk_t chunk;
    assert(!mc_chunk_init(&chunk, cx, cz, 0));
    assert(!mc_chunk_set_block(&chunk, 15, 319, 15, state));
    blob_t raw = {0};
    assert(!mc_anvil_encode_chunk_nbt(&chunk, &entities, &raw.data, &raw.size));
    mc_chunk_destroy(&chunk);
    /* Append an opaque NBT field: non-target payloads must survive byte-for-byte. */
    static const uint8_t field[] = {7, 0, 6, 'o', 'p',  'a', 'q',  'u',  'e',
                                    0, 0, 0, 4,   0xff, 0,   0x7f, 0x42, 0};
    raw.data = realloc(raw.data, raw.size + sizeof(field));
    assert(raw.data);
    memcpy(raw.data + raw.size - 1, field, sizeof(field));
    raw.size += sizeof(field) - 1;
    if (extra) {
        /* A valid incompressible array forces a larger sector allocation. */
        size_t length = 9000, pos = raw.size - 1;
        raw.data = realloc(raw.data, raw.size + length + 10);
        assert(raw.data);
        uint8_t prefix[] = {7, 0, 3, 'b', 'i', 'g', 0, 0, 0, 0};
        prefix[8] = (uint8_t)(length >> 8);
        prefix[9] = (uint8_t)length;
        memcpy(raw.data + pos, prefix, sizeof(prefix));
        pos += sizeof(prefix);
        uint32_t r = 12345;
        for (size_t i = 0; i < length; i++) {
            r ^= r << 13;
            r ^= r >> 17;
            r ^= r << 5;
            raw.data[pos++] = (uint8_t)r;
        }
        raw.data[pos++] = 0;
        raw.size = pos;
    }
    return raw;
}
static void expect_raw_at(const char *path, int slot, int cx, blob_t expected) {
    blob_t actual = {0};
    assert(!mc_anvil_read_chunk_nbt(path, slot, 0, &actual.data, &actual.size));
    assert(equal(actual, expected));
    free(actual.data);
    mc_chunk_t chunk;
    mc_arena_t arena;
    assert(!mc_chunk_init(&chunk, cx, -32, 0));
    assert(!mc_arena_init(&arena, 2u * 1024u * 1024u));
    assert(!mc_anvil_load_chunk(path, cx, -32, &chunk, &entities, &arena));
    mc_global_state_id_t value = mc_chunk_get_block(&chunk, 15, 319, 15);
    assert(value == stone || value == stair);
    mc_arena_destroy(&arena);
    mc_chunk_destroy(&chunk);
}
static void expect_raw(const char *path, int slot, blob_t expected) {
    expect_raw_at(path, slot, -32 + slot, expected);
}
static int write_bin(const char *world, mc_global_state_id_t state) {
    mc_chunk_t chunk;
    assert(!mc_chunk_init(&chunk, -32, -32, 0));
    assert(!mc_chunk_set_block(&chunk, 15, 319, 15, state));
    int rc = mc_chunk_store_write(world, &chunk, &entities);
    mc_chunk_destroy(&chunk);
    return rc;
}
static void expect_bin(const char *world, mc_global_state_id_t state) {
    mc_chunk_t chunk;
    assert(!mc_chunk_store_read(world, -32, -32, &chunk, &entities));
    assert(mc_chunk_get_block(&chunk, 15, 319, 15) == state);
    mc_chunk_destroy(&chunk);
}
static void reset_faults(void) {
    fault = NONE;
    interruption = RUN;
    remaining = 0;
    temporary = directory = -1;
    replaced = 0;
}
static void path_join(char *out, size_t cap, const char *a, const char *b) {
    int n = snprintf(out, cap, "%s/%s", a, b);
    assert(n > 0 && (size_t)n < cap);
}
typedef struct {
    const char *path;
    int slot;
    blob_t data;
    int second;
} writer_t;
static void *write_thread(void *arg) {
    writer_t *w = arg;
    is_second = w->second;
    assert(!mc_anvil_write_chunk_nbt(w->path, w->slot, 0, w->data.data, w->data.size));
    if (w->second)
        atomic_store(&second_done, true);
    return NULL;
}

static int write_region_group(const char *region,blob_t *old,blob_t newer,blob_t second) {
    mc_anvil_chunk_write_t writes[]={
        {0,newer.data,newer.size,old[0].data,old[0].size},
        {1,second.data,second.size,old[1].data,old[1].size}
    };
    return mc_anvil_write_batch_expected(region,writes,2);
}

int main(void) {
    const char *base = getenv("MC_ATOMIC_TEST_ROOT");
    if (!base)
        base = "/tmp";
    char root[4096];
    path_join(root, sizeof(root), base, "atomic-save-XXXXXX");
    assert(mkdtemp(root));
    printf("atomic persistence artifacts: %s\n", root);
    stone = mc_global_state_id_from_key("minecraft:stone", UINT32_MAX);
    stair = mc_global_state_id_from_key(
        "minecraft:oak_stairs[facing=east,half=top,shape=inner_right,waterlogged=true]", UINT32_MAX);
    assert(stone != UINT32_MAX && stair != UINT32_MAX);
    mc_be_store_init(&entities);
    char region[4096], world[4096], bin[4096];
    path_join(region, sizeof(region), root, "r.-1.-1.mca");
    path_join(world, sizeof(world), root, "world");
    path_join(bin, sizeof(bin), world, "chunks/c_-32_-32.bin");
    blob_t old[3], newer = make_raw(-32, -32, stair, 0), large = make_raw(-32, -32, stair, 1);
    blob_t second_new = make_raw(-31, -32, stair, 1);
    for (int i = 0; i < 3; i++) {
        old[i] = make_raw(-32 + i, -32, stone, 0);
        assert(!mc_anvil_write_chunk_nbt(region, i, 0, old[i].data, old[i].size));
    }
    assert(!chmod(region, 0640));
    assert(!write_bin(world, stone));
    blob_t region_before = read_file(region), bin_before = read_file(bin);

    for (int kind = 0; kind < 3; kind++) {
        const char *target = kind ? region : bin;
        blob_t before = kind ? region_before : bin_before;
        for (int point = BEFORE_TEMP; point <= AFTER_DIR_SYNC; point++) {
            restore(target, before);
            pid_t pid = fork();
            assert(pid >= 0);
            if (!pid) {
                interruption = point;
                if (kind==2)
                    (void)write_region_group(region,old,newer,second_new);
                else if (kind)
                    (void)mc_anvil_write_chunk_nbt(region, 0, 0, newer.data, newer.size);
                else
                    (void)write_bin(world, stair);
                _exit(73);
            }
            int status;
            assert(waitpid(pid, &status, 0) == pid);
            assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
            blob_t after = read_file(target);
            if (point <= BEFORE_RENAME)
                assert(equal(before, after));
            if (kind) {
                expect_raw(region, 0, point <= BEFORE_RENAME ? old[0] : newer);
                expect_raw(region, 1, kind==2 && point>BEFORE_RENAME ? second_new : old[1]);
                expect_raw(region, 2, old[2]);
            } else
                expect_bin(world, point <= BEFORE_RENAME ? stone : stair);
            printf("crash kind=%s point=%d readable=1 old_intact=%d\n", kind==2?"region-group":kind ? "region" : "bin", point,
                   equal(before, after));
            free(after.data);
        }
    }

    /* Orphans from SIGKILL are deliberately retained and never promoted. */
    const int errors[] = {EINTR, EIO, ENOSPC, EDQUOT, 0, -1};
    for (int kind = 0; kind < 3; kind++) {
        const char *target = kind ? region : bin;
        blob_t before = kind ? region_before : bin_before;
        for (int op = CREATE_TEMP; op <= LOCK_REGION; op++) {
            if (!kind && (op == PWRITE_TEMP || op == READ_REGION || op == STAT_REGION || op == LOCK_REGION))
                continue;
            for (size_t e = 0; e < sizeof(errors) / sizeof(errors[0]); e++) {
                int err = errors[e];
                if (err <= 0 && op != WRITE_TEMP && op != PWRITE_TEMP && op != READ_REGION)
                    continue;
                restore(target, before);
                reset_faults();
                char temp_dir[4096];
                if (kind)
                    snprintf(temp_dir, sizeof(temp_dir), "%s", root);
                else
                    path_join(temp_dir, sizeof(temp_dir), world, "chunks");
                int before_temps = count_temporaries(temp_dir);
                fault = op;
                injected_errno = err;
                remaining = 1;
                int rc = kind==2 ? write_region_group(region,old,newer,second_new) : kind ? mc_anvil_write_chunk_nbt(region, 0, 0, newer.data, newer.size)
                              : write_bin(world, stair);
                assert(!remaining);
                int result_errno = errno;
                int success = err == -1 || (err == EINTR && op != CLOSE_FILE && op != CLOSE_DIR);
                int uncertain = !success && (op == SYNC_DIR || op == CLOSE_DIR);
                assert(rc == (success ? 0 : uncertain ? MC_SAVE_DURABILITY_UNCERTAIN : -1));
                if (!success)
                    assert(result_errno == (err ? err : EIO));
                reset_faults();
                assert(count_temporaries(temp_dir) == before_temps);
                blob_t after = read_file(target);
                if (!success && !uncertain)
                    assert(equal(before, after));
                if (kind) {
                    expect_raw(region, 0, success || uncertain ? newer : old[0]);
                    expect_raw(region, 1, kind==2 && (success || uncertain) ? second_new : old[1]);
                    expect_raw(region, 2, old[2]);
                } else
                    expect_bin(world, success || uncertain ? stair : stone);
                printf("error kind=%s op=%d injected=%d rc=%d readable=1\n", kind==2?"region-group":kind ? "region" : "bin", op, err,
                       rc);
                free(after.data);
            }
        }
    }
    /* Failure of the free-sector bitmap must not select another live chunk. */
    for (int skip = 0; skip < 3; skip++) {
        restore(region, region_before);
        fault = ALLOCATE;
        injected_errno = ENOMEM;
        remaining = 1;
        allocation_skip = skip;
        assert(mc_anvil_write_chunk_nbt(region, 0, 0, large.data, large.size) == -1 && !remaining);
        reset_faults();
        blob_t after = read_file(region);
        assert(equal(after, region_before));
        free(after.data);
    }

    /* Cooperative writers, a preparation-time update, and an old open inode. */
    restore(region, region_before);
    int old_fd = open(region, O_RDONLY);
    assert(old_fd >= 0);
    assert(!sem_init(&first_preparing, 0, 0) && !sem_init(&release_first, 0, 0) &&
           !sem_init(&second_waiting, 0, 0));
    atomic_store(&block_first, true);
    atomic_store(&second_done, false);
    writer_t a = {region, 0, newer, 0}, b = {region, 1, second_new, 1};
    pthread_t first, second;
    assert(!pthread_create(&first, NULL, write_thread, &a));
    while (sem_wait(&first_preparing))
        assert(errno == EINTR);
    assert(!pthread_create(&second, NULL, write_thread, &b));
    while (sem_wait(&second_waiting))
        assert(errno == EINTR);
    assert(!atomic_load(&second_done));
    assert(!sem_post(&release_first));
    assert(!pthread_join(first, NULL) && !pthread_join(second, NULL));
    expect_raw(region, 0, newer);
    expect_raw(region, 1, second_new);
    expect_raw(region, 2, old[2]);
    uint8_t *old_image = malloc(region_before.size);
    assert(old_image);
    assert(!mc_file_pread_all(old_fd, old_image, region_before.size, 0));
    assert(!memcmp(old_image, region_before.data, region_before.size));
    free(old_image);
    assert(!close(old_fd));
    struct stat st;
    assert(!stat(region, &st) && (st.st_mode & 0777) == 0640);
    assert(!sem_destroy(&first_preparing) && !sem_destroy(&release_first) && !sem_destroy(&second_waiting));

    /* The same stable lock serializes separate processes, not just threads. */
    restore(region, region_before);
    int gate[2];
    assert(!pipe(gate));
    pid_t children[2];
    for (int i = 0; i < 2; i++) {
        children[i] = fork();
        assert(children[i] >= 0);
        if (!children[i]) {
            assert(!close(gate[1]));
            char go;
            ssize_t n;
            do {
                n = read(gate[0], &go, 1);
            } while (n < 0 && errno == EINTR);
            assert(n == 1);
            assert(!close(gate[0]));
            blob_t data = i ? second_new : newer;
            assert(!mc_anvil_write_chunk_nbt(region, i, 0, data.data, data.size));
            _exit(0);
        }
    }
    assert(!close(gate[0]));
    assert(!mc_file_write_all(gate[1], "go", 2));
    assert(!close(gate[1]));
    for (int i = 0; i < 2; i++) {
        int status;
        assert(waitpid(children[i], &status, 0) == children[i]);
        assert(WIFEXITED(status) && !WEXITSTATUS(status));
    }
    expect_raw(region, 0, newer);
    expect_raw(region, 1, second_new);
    expect_raw(region, 2, old[2]);

    /* Invalid offsets, overlaps, truncated records, external .mcc and codecs. */
    for (int bad = 0; bad < 5; bad++) {
        restore(region, region_before);
        int fd = open(region, O_RDWR);
        assert(fd >= 0);
        uint8_t data[5];
        if (bad == 0) {
            uint8_t overlap[4] = {0, 0, 2, 1};
            assert(!mc_file_pwrite_all(fd, overlap, 4, 4));
        }
        if (bad == 1) {
            uint8_t outside[4] = {0, 0, 99, 1};
            assert(!mc_file_pwrite_all(fd, outside, 4, 0));
        }
        if (bad == 2) {
            uint8_t length[4] = {0x7f, 0xff, 0xff, 0xff};
            assert(!mc_file_pwrite_all(fd, length, 4, 8192));
        }
        if (bad >= 3) {
            assert(!mc_file_pread_all(fd, data, 5, 8192));
            data[4] = bad == 3 ? 0x82 : 4;
            assert(!mc_file_pwrite_all(fd, data, 5, 8192));
        }
        assert(!close(fd));
        blob_t damaged = read_file(region);
        char external[4096];
        path_join(external, sizeof(external), root, "c.-32.-32.mcc");
        restore(external, old[0]);
        assert(mc_anvil_write_chunk_nbt(region, 2, 0, newer.data, newer.size) == -1);
        blob_t after = read_file(region), mcc = read_file(external);
        assert(equal(damaged, after) && equal(mcc, old[0]));
        free(damaged.data);
        free(after.data);
        free(mcc.data);
    }
    /* Distinct region files across the negative/zero region boundary. */
    restore(region, region_before);
    char eastern[4096];
    path_join(eastern, sizeof(eastern), root, "r.0.-1.mca");
    blob_t west = make_raw(-1, -32, stair, 0), east = make_raw(0, -32, stone, 0),
           east_new = make_raw(0, -32, stair, 0);
    assert(!mc_anvil_write_chunk_nbt(region, 31, 0, west.data, west.size));
    assert(!mc_anvil_write_chunk_nbt(eastern, 0, 0, east.data, east.size));
    expect_raw_at(eastern, 0, 0, east);
    expect_raw_at(region, 31, -1, west);
    assert(!mc_anvil_write_chunk_nbt(eastern, 0, 0, east_new.data, east_new.size));
    expect_raw_at(eastern, 0, 0, east_new);
    expect_raw_at(region, 31, -1, west);
    expect_raw(region, 1, old[1]);
    free(west.data);
    free(east.data);
    free(east_new.data);

    /* Normal failure cleanup has no temporaries; crash orphans stay elsewhere. */
    char clean[4096];
    path_join(clean, sizeof(clean), root, "clean");
    assert(!mkdir(clean, 0755));
    char clean_region[4096];
    path_join(clean_region, sizeof(clean_region), clean, "r.0.0.mca");
    fault = SYNC_FILE;
    injected_errno = ENOSPC;
    remaining = 1;
    assert(mc_anvil_write_chunk_nbt(clean_region, 0, 0, newer.data, newer.size) == -1);
    reset_faults();
    assert(access(clean_region, F_OK) != 0 && errno == ENOENT);
    no_temporaries(clean);
    /* A failed or uncertain live flush retains dirtiness. A later tick-owned
     * mutation must win over the earlier snapshot when the flush is retried. */
    char live[4096];
    path_join(live, sizeof(live), root, "live-world");
    mc_world_t *w = mc_world_create_with_workers(live, 0, 1);
    assert(w);
    int32_t value = 0;
    int ready = 0;
    for (int i = 0; i < 2000; i++) {
        mc_world_tick(w, 0);
        if (!mc_world_get_block_ready(w, -497, 319, -497, &value)) {
            ready = 1;
            break;
        }
        struct timespec t = {0, 1000000};
        nanosleep(&t, NULL);
    }
    assert(ready && !mc_world_flush_block(w, -497, 319, -497));
    for (int op = SYNC_FILE; op <= SYNC_DIR; op += 2) {
        assert(!mc_world_set_block(w, -497, 319, -497, (int32_t)stair));
        fault = op;
        injected_errno = EIO;
        remaining = 1;
        assert(mc_world_flush_block(w, -497, 319, -497) == -1 && !remaining);
        reset_faults();
        mc_chunk_t *current = mc_world_get_chunk(w, -32, -32, UINT32_MAX);
        assert(current && current->dirty);
        uint64_t revision = current->revision;
        assert(!mc_world_set_block(w, -497, 319, -497, (int32_t)stone));
        assert(current->revision > revision && current->dirty);
        assert(!mc_world_flush_block(w, -497, 319, -497) && !current->dirty);
        expect_bin(live, stone);
    }
    mc_world_destroy(w);
    char live_bin[4096];
    path_join(live_bin, sizeof(live_bin), live, "chunks/c_-32_-32.bin");
    blob_t live_before = read_file(live_bin);
    snprintf(load_failure_path, sizeof(load_failure_path), "%s", live_bin);
    atomic_store(&fail_load_once, true);
    w = mc_world_create_with_workers(live, 0, 1);
    assert(w);
    (void)mc_world_get_block_ready(w, -497, 319, -497, &value);
    for (int i = 0; i < 2000 && atomic_load(&fail_load_once); i++) {
        mc_world_tick(w, 0);
        struct timespec delay = {0, 1000000};
        nanosleep(&delay, NULL);
    }
    assert(!atomic_load(&fail_load_once));
    for (int i = 0; i < 50; i++) {
        mc_world_tick(w, 0);
        struct timespec delay = {0, 1000000};
        nanosleep(&delay, NULL);
    }
    assert(mc_world_get_block_ready(w, -497, 319, -497, &value) != 0);
    assert(mc_world_flush_block(w, -497, 319, -497) == -1);
    mc_world_destroy(w);
    blob_t live_after = read_file(live_bin);
    assert(equal(live_before, live_after));
    free(live_after.data);
    free(live_before.data);
    w = mc_world_create_with_workers(live, 0, 1);
    assert(w);
    ready = 0;
    for (int i = 0; i < 2000; i++) {
        mc_world_tick(w, 0);
        if (!mc_world_get_block_ready(w, -497, 319, -497, &value)) {
            ready = 1;
            break;
        }
        struct timespec delay = {0, 1000000};
        nanosleep(&delay, NULL);
    }
    assert(ready && value == (int32_t)stone);
    mc_world_destroy(w);
    free(region_before.data);
    free(bin_before.data);
    free(newer.data);
    free(large.data);
    free(second_new.data);
    for (int i = 0; i < 3; i++)
        free(old[i].data);
    mc_be_store_destroy(&entities);
    puts("test_atomic_persistence: ok");
    return 0;
}
