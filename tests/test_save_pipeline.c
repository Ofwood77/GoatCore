/* S4 snapshots, generation acknowledgements and all-or-none region batches. */
#include "mc_world_storage.h"
#include "mc_anvil.h"
#include "mc_world.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <zlib.h>
#include <signal.h>
#include <sys/wait.h>
static _Atomic unsigned checks;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        checks++;                                                                                            \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "%s:%d %s errno=%d\n", __FILE__, __LINE__, #x, errno);                           \
            abort();                                                                                         \
        }                                                                                                    \
    } while (0)
static _Atomic int fail_parent;
static _Thread_local bool fail_malloc, fail_calloc;
static _Thread_local size_t fail_malloc_size;
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__wrap_malloc(size_t n) {
    if (fail_malloc || (fail_malloc_size && n == fail_malloc_size)) {
        fail_malloc = false;
        fail_malloc_size = 0;
        errno = ENOMEM;
        return NULL;
    }
    return __real_malloc(n);
}
void *__wrap_calloc(size_t n, size_t bytes) {
    if (fail_calloc) {
        fail_calloc = false;
        errno = ENOMEM;
        return NULL;
    }
    return __real_calloc(n, bytes);
}
static _Atomic unsigned file_syncs, dir_syncs;
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static bool gate_enabled, gate_entered;
static int gate_kind;
static void gate_checkpoint(bool directory, bool after) {
    int kind = directory ? (after ? 3 : 2) : (after ? 1 : 0);
    pthread_mutex_lock(&gate_lock);
    if (gate_enabled && gate_kind == kind) {
        gate_entered = true;
        pthread_cond_broadcast(&gate_cv);
        while (gate_enabled)
            pthread_cond_wait(&gate_cv, &gate_lock);
    }
    pthread_mutex_unlock(&gate_lock);
}
int __real_fsync(int fd);
int __wrap_fsync(int fd) {
    struct stat st;
    CHECK(fstat(fd, &st) == 0);
    if (S_ISDIR(st.st_mode))
        dir_syncs++;
    else
        file_syncs++;
    if (fail_parent && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (fail_parent > 0)
            fail_parent--;
        errno = EIO;
        return -1;
    }
    gate_checkpoint(S_ISDIR(st.st_mode), false);
    int rc = __real_fsync(fd);
    if (!rc)
        gate_checkpoint(S_ISDIR(st.st_mode), true);
    return rc;
}
static mc_world_storage_t *store(char name[64]) {
    strcpy(name, "/tmp/goatcore-save-s4.XXXXXX");
    CHECK(mkdtemp(name));
    mc_world_storage_t *s = NULL;
    CHECK(mc_storage_open(name, MC_STORAGE_ANVIL_ATOMIC, &s) == 0);
    return s;
}
static void begin(mc_chunk_t *live, mc_chunk_t *snap) {
    size_t bytes = 0;
    CHECK(!live->save_pending);
    CHECK(mc_chunk_save_snapshot(live, snap, 64u * 1024u * 1024u, &bytes) == 0);
    CHECK(bytes >= sizeof(*snap));
    live->save_pending = true;
    live->inflight_generation = snap->save_generation;
}
static void point(mc_world_storage_t *s, int cx, int cz, uint32_t expected) {
    mc_chunk_t c;
    CHECK(mc_storage_read(s, cx, cz, &c, NULL) == 0);
    CHECK(c.durable_generation == c.save_generation);
    CHECK(mc_chunk_get_block(&c, 8, 64, 8) == expected);
    mc_chunk_destroy(&c);
}
static void snapshots(void) {
    char root[64];
    mc_world_storage_t *s = store(root);
    mc_chunk_t live, a, b, old;
    CHECK(mc_chunk_init(&live, -1, -1, 0) == 0);
    CHECK(mc_storage_write(s, &live, NULL) == 0);
    CHECK(mc_chunk_set_block(&live, 8, 64, 8, 1) == 0);
    live.dirty = true;
    begin(&live, &a);
    size_t n = 0;
    CHECK(mc_chunk_save_snapshot(&live, &old, 64u * 1024u * 1024u, &n) == 0);
    CHECK(a.sections[8].words != live.sections[8].words);
    CHECK(a.storage_nbt != live.storage_nbt);
    CHECK(mc_chunk_set_block(&live, 8, 64, 8, mc_global_state_id_from_key("minecraft:dirt", UINT32_MAX)) ==
          0);
    CHECK(mc_chunk_get_block(&a, 8, 64, 8) == 1);
    uint8_t *captured = NULL, *written = NULL;
    size_t captured_n = 0, written_n = 0;
    CHECK(mc_anvil_encode_native(&a, NULL, &captured, &captured_n) == 0);
    mc_chunk_t *list[] = {&a};
    CHECK(mc_storage_write_region(s, list, 1) == 0);
    CHECK(mc_anvil_encode_native(&a, NULL, &written, &written_n) == 0);
    CHECK(captured_n == written_n && !memcmp(captured, written, captured_n));
    free(captured);
    free(written);
    CHECK(mc_chunk_complete_save(&live, &a, 0) == 0);
    CHECK(live.dirty && !live.save_pending && live.durable_generation < live.save_generation);
    point(s, -1, -1, 1);
    mc_chunk_destroy(&a);
    begin(&live, &b);
    list[0] = &b;
    CHECK(mc_storage_write_region(s, list, 1) == 0);
    CHECK(mc_chunk_complete_save(&live, &b, 0) == 0);
    CHECK(!live.dirty && live.durable_generation == live.save_generation);
    point(s, -1, -1, mc_global_state_id_from_key("minecraft:dirt", UINT32_MAX));
    list[0] = &old;
    CHECK(mc_storage_write_region(s, list, 1) == -1 && errno == ESTALE);
    CHECK(mc_chunk_complete_save(&live, &old, 0) == -1 && errno == ESTALE);
    mc_chunk_destroy(&old);
    mc_chunk_destroy(&b);
    CHECK(mc_chunk_set_block(&live, 8, 64, 8, 1) == 0);
    live.dirty = true;
    begin(&live, &a);
    list[0] = &a;
    fail_parent = 1;
    CHECK(mc_storage_write_region(s, list, 1) == MC_SAVE_DURABILITY_UNCERTAIN);
    CHECK(mc_chunk_complete_save(&live, &a, MC_SAVE_DURABILITY_UNCERTAIN) == 0 && live.dirty);
    point(s, -1, -1, 1);
    mc_chunk_destroy(&a);
    begin(&live, &a);
    list[0] = &a;
    CHECK(mc_storage_write_region(s, list, 1) == 0);
    CHECK(mc_chunk_complete_save(&live, &a, 0) == 0 && !live.dirty);
    mc_chunk_destroy(&a);
    CHECK(mc_chunk_save_snapshot(&live, &a, 1, &n) == -1 && errno == EFBIG);
    mc_chunk_destroy(&a);
    live.save_generation = UINT64_MAX;
    CHECK(mc_chunk_set_block(&live, 8, 64, 8, 0) == -1 && errno == EOVERFLOW);
    CHECK(mc_chunk_get_block(&live, 8, 64, 8) == 1);
    CHECK(mc_chunk_mark_dirty(&live) == -1 && errno == EOVERFLOW);
    mc_chunk_destroy(&live);
    mc_storage_close(s);
}
static void region(unsigned count) {
    char root[64];
    mc_world_storage_t *s = store(root);
    mc_chunk_t *live = calloc(count, sizeof(*live));
    mc_chunk_t **list = calloc(count, sizeof(*list));
    CHECK(live && list);
    for (unsigned i = 0; i < count; i++) {
        CHECK(mc_chunk_init(&live[i], -32 + (int)i % 32, -32 + (int)i / 32, 0) == 0);
        list[i] = &live[i];
        CHECK(mc_chunk_set_block(&live[i], 8, 64, 8, 1) == 0);
    }
    CHECK(mc_storage_write_region(s, list, count) == 0);
    size_t stale_len = live[1].storage_nbt_len;
    uint8_t *stale = malloc(stale_len);
    CHECK(stale);
    memcpy(stale, live[1].storage_nbt, stale_len);
    mc_chunk_t changed[100];
    mc_chunk_t *batch[100];
    for (unsigned i = 0; i < 100; i++) {
        CHECK(mc_chunk_set_block(&live[i], 8, 64, 8, 0) == 0);
        begin(&live[i], &changed[i]);
        batch[i] = &changed[i];
    }
    CHECK(mc_storage_write_region(s, batch, 100) == 0);
    for (unsigned i = 0; i < 100; i++) {
        CHECK(mc_chunk_complete_save(&live[i], &changed[i], 0) == 0);
        mc_chunk_destroy(&changed[i]);
    }
    for (unsigned i = 0; i < count; i++)
        point(s, live[i].cx, live[i].cz, i < 100 ? 0 : 1);
    /* One stale source aborts the complete region publication. */
    mc_chunk_t a, b;
    begin(&live[0], &a);
    begin(&live[1], &b);
    CHECK(mc_chunk_set_block(&a, 8, 64, 8, 1) == 0);
    CHECK(mc_chunk_set_block(&b, 8, 64, 8, 1) == 0);
    free(b.storage_nbt);
    b.storage_nbt = stale;
    b.storage_nbt_len = stale_len;
    mc_chunk_t *bad[] = {&a, &b};
    CHECK(mc_storage_write_region(s, bad, 2) == -1 && errno == ESTALE);
    point(s, live[0].cx, live[0].cz, 0);
    point(s, live[1].cx, live[1].cz, 0);
    mc_chunk_t *dupe[] = {&a, &a};
    CHECK(mc_storage_write_region(s, dupe, 2) == -1 && errno == EINVAL);
    b.cx = 0;
    CHECK(mc_storage_write_region(s, bad, 2) == -1 && errno == EINVAL);
    mc_chunk_destroy(&a);
    mc_chunk_destroy(&b);
    for (unsigned i = 0; i < count; i++)
        mc_chunk_destroy(&live[i]);
    free(live);
    free(list);
    mc_storage_close(s);
}
static void world_point(const char *root, int cx, int cz, uint32_t expected) {
    char path[4096];
    int32_t rx, rz;
    unsigned slot;
    mc_region_coordinates(cx, cz, &rx, &rz, &slot);
    CHECK(snprintf(path, sizeof(path), "%s/dimensions/minecraft/overworld/region/r.%d.%d.mca", root, rx, rz) <
          (int)sizeof(path));
    uint8_t *raw = NULL;
    size_t n = 0;
    mc_chunk_t c;
    CHECK(mc_anvil_read_chunk_nbt(path, slot % 32, slot / 32, &raw, &n) == 0);
    CHECK(mc_anvil_decode_native(raw, n, cx, cz, &c) == 0);
    CHECK(mc_chunk_get_block(&c, 8, 200, 8) == expected);
    free(raw);
    mc_chunk_destroy(&c);
}
static void integrated(void) {
    char root[64];
    strcpy(root, "/tmp/goatcore-save-s4.XXXXXX");
    CHECK(mkdtemp(root));
    mc_world_t *w = mc_world_create_with_storage(root, 0, 1, MC_STORAGE_ANVIL_ATOMIC);
    CHECK(w);
    CHECK(mc_world_configure_saves(w, MC_SAVE_BATCHED) == 0);
    CHECK(mc_world_configure_saves(w, MC_SAVE_ASYNC) == -1 && errno == EINVAL);
    mc_chunk_t *c[3] = {0};
    for (unsigned i = 0; i < 3; i++)
        (void)mc_world_get_chunk(w, -3 + (int)i, -1, 0);
    for (unsigned tries = 0; tries < 2000; tries++) {
        mc_world_tick(w, tries * 50);
        for (unsigned i = 0; i < 3; i++)
            c[i] = mc_world_get_chunk(w, -3 + (int)i, -1, 0);
        if (c[0] && c[1] && c[2])
            break;
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    CHECK(c[0] && c[1] && c[2]);
    CHECK(mc_world_flush_saves(w) == 0);
    uint32_t dirt = mc_global_state_id_from_key("minecraft:dirt", UINT32_MAX);
    for (unsigned i = 0; i < 100; i++) {
        CHECK(mc_world_set_block(w, -40, 200, -8, (i % 2) ? 1 : (int)dirt) == 0);
        CHECK(mc_world_request_block_save(w, -40, 200, -8) == 0);
    }
    CHECK(mc_world_set_block(w, -24, 200, -8, 1) == 0);
    CHECK(mc_world_set_block(w, -8, 200, -8, 1) == 0);
    CHECK(c[0]->dirty && c[1]->dirty && c[2]->dirty);
    world_point(root, -3, -1, 0); /* Requested is NOT durable until the flush. */
    file_syncs = dir_syncs = 0;
    mc_world_tick_stats_t stats;
    mc_world_tick_profiled(w, 100000, &stats);
    CHECK(stats.saves_succeeded == 3 && file_syncs == 1 && dir_syncs == 1);
    for (unsigned i = 0; i < 3; i++) {
        CHECK(!c[i]->dirty && !c[i]->save_pending && c[i]->durable_generation == c[i]->save_generation);
        world_point(root, -3 + (int)i, -1, 1);
    }
    CHECK(mc_world_set_block(w, -40, 200, -8, (int)dirt) == 0);
    fail_parent = 1;
    mc_world_tick(w, 100050);
    CHECK(c[0]->dirty && !c[0]->save_pending);
    CHECK(mc_world_set_block(w, -24, 200, -8, 0) == -1); /* Failure latch blocks mutations. */
    CHECK(mc_world_flush_saves(w) == 0 && !c[0]->dirty);
    world_point(root, -3, -1, dirt);
    CHECK(mc_world_set_block(w, -24, 200, -8, 0) == 0);
    mc_world_destroy(w);
    world_point(root, -2, -1, 0);
    char binroot[64];
    strcpy(binroot, "/tmp/goatcore-save-s4.XXXXXX");
    CHECK(mkdtemp(binroot));
    w = mc_world_create_with_storage(binroot, 0, 1, MC_STORAGE_BIN);
    CHECK(w);
    CHECK(mc_world_configure_saves(w, MC_SAVE_BATCHED) == -1 && errno == EINVAL);
    CHECK(mc_world_save_mode(w) == MC_SAVE_IMMEDIATE);
    mc_world_destroy(w);
}
static void arm(void) {
    pthread_mutex_lock(&gate_lock);
    gate_enabled = true;
    gate_kind = 0;
    gate_entered = false;
    pthread_mutex_unlock(&gate_lock);
}
static void entered(void) {
    struct timespec until;
    CHECK(clock_gettime(CLOCK_REALTIME, &until) == 0);
    until.tv_sec += 10;
    pthread_mutex_lock(&gate_lock);
    while (!gate_entered)
        CHECK(pthread_cond_timedwait(&gate_cv, &gate_lock, &until) == 0);
    pthread_mutex_unlock(&gate_lock);
}
static void release(void) {
    pthread_mutex_lock(&gate_lock);
    gate_enabled = false;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate_lock);
}
static void asynchronous(void) {
    char root[64];
    mc_world_storage_t *s = store(root);
    mc_chunk_t live;
    CHECK(mc_chunk_init(&live, -1, -1, 0) == 0);
    CHECK(mc_storage_write(s, &live, NULL) == 0);
    CHECK(mc_chunk_set_block(&live, 8, 64, 8, 1) == 0);
    live.dirty = true;
    mc_save_io_t *io = NULL;
    CHECK(mc_save_io_create(s, &io) == 0);
    mc_save_batch_t *batch = calloc(1, sizeof(*batch));
    CHECK(batch);
    CHECK(mc_save_batch_add(batch, &live) == 0);
    live.save_pending = true;
    live.inflight_generation = live.save_generation;
    arm();
    CHECK(mc_save_io_submit(io, batch) == 0);
    entered();
    CHECK(mc_save_io_busy(io) && mc_save_io_take(io, false) == NULL);
    uint32_t dirt = mc_global_state_id_from_key("minecraft:dirt", UINT32_MAX);
    CHECK(mc_chunk_set_block(&live, 8, 64, 8, dirt) == 0);
    mc_save_batch_t *second = calloc(1, sizeof(*second));
    CHECK(second);
    CHECK(mc_save_batch_add(second, &live) == 0);
    CHECK(mc_save_io_submit(io, second) == -1 && errno == EAGAIN);
    mc_save_batch_destroy(second);
    release();
    batch = mc_save_io_take(io, true);
    CHECK(batch && batch->results[0] == 0);
    CHECK(mc_chunk_complete_save(&live, &batch->chunks[0], 0) == 0 && live.dirty);
    mc_save_batch_destroy(batch);
    point(s, -1, -1, 1);
    batch = calloc(1, sizeof(*batch));
    CHECK(batch && mc_save_batch_add(batch, &live) == 0);
    live.save_pending = true;
    live.inflight_generation = live.save_generation;
    arm();
    fail_parent = 1;
    CHECK(mc_save_io_submit(io, batch) == 0);
    entered();
    CHECK(mc_chunk_set_block(&live, 8, 64, 8, 0) == 0);
    release();
    batch = mc_save_io_take(io, true);
    CHECK(batch && batch->results[0] == MC_SAVE_DURABILITY_UNCERTAIN);
    uint64_t durable = live.durable_generation;
    CHECK(mc_chunk_complete_save(&live, &batch->chunks[0], MC_SAVE_DURABILITY_UNCERTAIN) == 0);
    CHECK(live.dirty && live.durable_generation == durable);
    mc_save_batch_destroy(batch);
    point(s, -1, -1, dirt);
    batch = calloc(1, sizeof(*batch));
    CHECK(batch && mc_save_batch_add(batch, &live) == 0);
    live.save_pending = true;
    live.inflight_generation = live.save_generation;
    CHECK(mc_save_io_submit(io, batch) == 0);
    mc_save_batch_t *completed = NULL;
    CHECK(mc_save_io_close(io, &completed) == 0 && completed == batch);
    CHECK(completed->results[0] == 0 && mc_chunk_complete_save(&live, &completed->chunks[0], 0) == 0 &&
          !live.dirty);
    mc_save_batch_destroy(completed);
    point(s, -1, -1, 0);
    mc_chunk_destroy(&live);
    mc_storage_close(s);
}
static void asynchronous_world(void) {
    char root[64];
    strcpy(root, "/tmp/goatcore-save-s4.XXXXXX");
    CHECK(mkdtemp(root));
    mc_world_t *w = mc_world_create_with_storage(root, 0, 1, MC_STORAGE_ANVIL_ATOMIC);
    CHECK(w);
    CHECK(mc_world_configure_saves(w, MC_SAVE_ASYNC) == 0);
    mc_chunk_t *c = NULL;
    for (unsigned i = 0; i < 2000; i++) {
        c = mc_world_get_chunk(w, -1, -1, 0);
        mc_world_tick(w, i * 50);
        if (c)
            break;
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    CHECK(c && mc_world_flush_saves(w) == 0);
    CHECK(mc_world_set_block(w, -8, 200, -8, 1) == 0);
    arm();
    mc_world_tick(w, 100000);
    entered();
    CHECK(c->save_pending);
    uint64_t captured = c->inflight_generation;
    uint32_t dirt = mc_global_state_id_from_key("minecraft:dirt", UINT32_MAX);
    CHECK(mc_world_set_block(w, -8, 200, -8, (int)dirt) == 0);
    CHECK(mc_world_request_block_save(w, -8, 200, -8) == 0);
    for (unsigned i = 0; i < 20; i++)
        mc_world_tick(w, 100050 + i * 50); /* Must not wait on I/O. */
    CHECK(c->save_pending && c->dirty && c->save_generation > captured);
    CHECK(mc_world_evict_outside(w, NULL, 0, 10) == 0 && c->evict_after_save);
    release();
    CHECK(mc_world_flush_saves(w) == 0);
    world_point(root, -1, -1, dirt); /* Both generations completed, eviction allowed. */
    c = NULL;
    for (unsigned i = 0; i < 2000; i++) {
        c = mc_world_get_chunk(w, -1, -1, 0);
        mc_world_tick(w, 200000 + i * 50);
        if (c)
            break;
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    CHECK(c && mc_chunk_get_block(c, 8, 200, 8) == dirt);
    CHECK(mc_world_set_block(w, -8, 200, -8, 1) == 0);
    mc_world_tick(w, 300000);
    CHECK(mc_world_set_block(w, -8, 200, -8, 0) == 0); /* New state before shutdown drain. */
    mc_world_destroy(w);
    world_point(root, -1, -1, 0);
}
static void allocation_failures(void) {
    mc_chunk_t source, out;
    CHECK(mc_chunk_init(&source, 0, 0, 0) == 0);
    CHECK(mc_chunk_set_block(&source, 8, 64, 8, 1) == 0);
    size_t bytes = 0;
    fail_malloc = true;
    CHECK(mc_chunk_save_snapshot(&source, &out, MC_SAVE_SNAPSHOT_BYTES, &bytes) == -1 && errno == ENOMEM);
    CHECK(!out.storage_nbt && !out.sections[8].words);
    mc_chunk_destroy(&out);
    CHECK(mc_chunk_get_block(&source, 8, 64, 8) == 1);
    size_t old_count = source.sections[8].word_count;
    source.sections[8].word_count = SIZE_MAX;
    CHECK(mc_chunk_save_snapshot(&source, &out, MC_SAVE_SNAPSHOT_BYTES, &bytes) == -1 && errno == EOVERFLOW);
    source.sections[8].word_count = old_count;
    mc_save_batch_t *batch = calloc(1, sizeof(*batch));
    CHECK(batch);
    batch->bytes = MC_SAVE_SNAPSHOT_BYTES;
    CHECK(mc_save_batch_add(batch, &source) == -1 && errno == EFBIG && batch->count == 0);
    batch->bytes = 0;
    for (unsigned i = 0; i < MC_SAVE_BATCH_CHUNKS; i++) {
        source.cx = (int32_t)i;
        CHECK(mc_save_batch_add(batch, &source) == 0);
    }
    CHECK(mc_save_batch_add(batch, &source) == -1 && errno == EAGAIN);
    mc_save_batch_destroy(batch);
    char root[64];
    mc_world_storage_t *store_ = store(root);
    source.cx = 0;
    CHECK(mc_storage_write(store_, &source, NULL) == 0);
    uint8_t *raw = NULL;
    size_t n = 0;
    CHECK(mc_anvil_encode_native(&source, NULL, &raw, &n) == 0);
    mc_anvil_chunk_write_t write = {0, raw, n, source.storage_nbt, source.storage_nbt_len};
    fail_malloc_size = compressBound((uLong)n);
    char path[4096];
    CHECK(snprintf(path, sizeof(path), "%s/dimensions/minecraft/overworld/region/r.0.0.mca", root) <
          (int)sizeof(path));
    CHECK(mc_anvil_write_batch_expected(path, &write, 1) == -1);
    CHECK(!fail_malloc_size && errno == ENOMEM);
    point(store_, 0, 0, 1);
    free(raw);
    mc_storage_close(store_);
    mc_chunk_destroy(&source);
}
static void failed_shutdown(void) {
    char root[64];
    strcpy(root, "/tmp/goatcore-save-s4.XXXXXX");
    CHECK(mkdtemp(root));
    mc_world_t *w = mc_world_create_with_storage(root, 0, 1, MC_STORAGE_ANVIL_ATOMIC);
    CHECK(w);
    CHECK(mc_world_configure_saves(w, MC_SAVE_BATCHED) == 0);
    mc_chunk_t *c = NULL;
    for (unsigned i = 0; i < 2000; i++) {
        c = mc_world_get_chunk(w, -1, -1, 0);
        mc_world_tick(w, i * 50);
        if (c)
            break;
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    CHECK(c && mc_world_flush_saves(w) == 0);
    CHECK(mc_world_set_block(w, -8, 200, -8, 1) == 0);
    fail_calloc = true;
    mc_world_tick(w, 100000);
    CHECK(!fail_calloc && c->dirty && !c->save_pending);
    CHECK(mc_world_set_block(w, -8, 200, -8, 0) == -1);
    CHECK(mc_world_flush_saves(w) == 0);
    CHECK(mc_world_set_block(w, -8, 200, -8, 0) == 0);
    fail_parent = -1;
    CHECK(mc_world_destroy_checked(w) == -1);
    fail_parent = 0;
    world_point(root, -1, -1, 0);
}
static void crash_child(const char *root, int stage) {
    CHECK(strncmp(root, "/tmp/goatcore-save-s4.", strlen("/tmp/goatcore-save-s4.")) == 0);
    char marker[4096];
    CHECK(snprintf(marker, sizeof(marker), "%s/.s4-crash-fixture", root) < (int)sizeof(marker));
    struct stat st;
    CHECK(lstat(marker, &st) == 0 && S_ISREG(st.st_mode));
    mc_world_t *w = mc_world_create_with_storage(root, 0, 1, MC_STORAGE_ANVIL_ATOMIC);
    CHECK(w);
    CHECK(mc_world_configure_saves(w, MC_SAVE_ASYNC) == 0);
    mc_chunk_t *c = NULL;
    for (unsigned i = 0; i < 2000; i++) {
        c = mc_world_get_chunk(w, -1, -1, 0);
        mc_world_tick(w, i * 50);
        mc_chunk_t *second = mc_world_get_chunk(w, -2, -1, 0), *third = mc_world_get_chunk(w, -3, -1, 0);
        if (c && second && third)
            break;
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    CHECK(c && !c->dirty && c->save_generation == 0);
    CHECK(mc_world_set_block(w, -8, 200, -8, 1) == 0);
    CHECK(mc_world_set_block(w, -24, 200, -8, 1) == 0);
    CHECK(mc_world_set_block(w, -40, 200, -8, 1) == 0);
    if (stage < 4) {
        arm();
        pthread_mutex_lock(&gate_lock);
        gate_kind = stage;
        pthread_mutex_unlock(&gate_lock);
        mc_world_tick(w, 200000);
        entered();
        CHECK(c->save_pending);
    } else if (stage == 5) {
        CHECK(mc_world_flush_saves(w) == 0 && !c->dirty);
    }
    if (stage != 4) {
        CHECK(mc_world_set_block(w, -8, 200, -8,
                                 (int)mc_global_state_id_from_key("minecraft:dirt", UINT32_MAX)) == 0);
        CHECK(c->dirty && c->save_generation > c->durable_generation);
    }
    /* The later dirty state is deliberately unconfirmed, never claimed durable. */
    kill(getpid(), SIGKILL);
    _exit(99);
}
static void async_crashes(const char *self) {
    for (int stage = 0; stage < 6; stage++) {
        char root[64];
        mc_world_storage_t *s = store(root);
        mc_chunk_t chunks_[3];
        mc_chunk_t *list[3];
        for (int i = 0; i < 3; i++) {
            CHECK(mc_chunk_init(&chunks_[i], -1 - i, -1, 0) == 0);
            list[i] = &chunks_[i];
        }
        CHECK(mc_storage_write_region(s, list, 3) == 0);
        for (int i = 0; i < 3; i++)
            mc_chunk_destroy(&chunks_[i]);
        mc_storage_close(s);
        char marker[4096];
        CHECK(snprintf(marker, sizeof(marker), "%s/.s4-crash-fixture", root) < (int)sizeof(marker));
        int fd = open(marker, O_CREAT | O_EXCL | O_WRONLY, 0600);
        CHECK(fd >= 0 && close(fd) == 0);
        char which[16];
        CHECK(snprintf(which, sizeof(which), "%d", stage) > 0);
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (!pid) {
            execl(self, self, "--crash-child", root, which, (char *)NULL);
            _exit(98);
        }
        int status;
        CHECK(waitpid(pid, &status, 0) == pid);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        uint32_t expected = stage < 2 || stage == 4 ? 0 : 1;
        for (unsigned pass = 0; pass < 2; pass++) {
            CHECK(mc_storage_open(root, MC_STORAGE_ANVIL_ATOMIC, &s) == 0);
            for (int i = 0; i < 3; i++) {
                mc_chunk_t restored;
                CHECK(mc_storage_read(s, -1 - i, -1, &restored, NULL) == 0);
                CHECK(mc_chunk_get_block(&restored, 8, 200, 8) == expected);
                CHECK(restored.save_generation == 0 && restored.durable_generation == 0);
                mc_chunk_destroy(&restored);
            }
            mc_storage_close(s);
        }
        printf("async crash stage=%d restored=%u later_dirty_not_acknowledged=1 repeat_read=1 chunks=3\n",
               stage, expected);
    }
}
int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--crash-child")) {
        crash_child(argv[2], atoi(argv[3]));
        return 99;
    }
    CHECK(argc == 1);
    allocation_failures();
    failed_shutdown();
    asynchronous();
    asynchronous_world();
    integrated();
    snapshots();
    region(256);
    region(1024);
    async_crashes(argv[0]);
    printf("test_save_pipeline: %u checks passed\n", checks);
    return 0;
}
