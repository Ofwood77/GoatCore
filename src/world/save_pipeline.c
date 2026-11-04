#include "mc_save_pipeline.h"
#include "mc_region_storage.h"
#include "mc_util.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
const char *mc_save_mode_name(mc_save_mode_t mode) {
    return mode == MC_SAVE_IMMEDIATE ? "immediate"
           : mode == MC_SAVE_BATCHED ? "batched"
           : mode == MC_SAVE_ASYNC   ? "async"
                                     : "invalid";
}
int mc_save_mode_parse(const char *name, mc_save_mode_t *mode) {
    if (name && mode) {
        if (!strcmp(name, "immediate")) {
            *mode = MC_SAVE_IMMEDIATE;
            return 0;
        }
        if (!strcmp(name, "batched")) {
            *mode = MC_SAVE_BATCHED;
            return 0;
        }
        if (!strcmp(name, "async")) {
            *mode = MC_SAVE_ASYNC;
            return 0;
        }
    }
    errno = EINVAL;
    return -1;
}
int mc_save_batch_add(mc_save_batch_t *batch, const mc_chunk_t *source) {
    if (!batch || !source || batch->count >= MC_SAVE_BATCH_CHUNKS || batch->bytes > MC_SAVE_SNAPSHOT_BYTES) {
        errno = EAGAIN;
        return -1;
    }
    for (size_t i = 0; i < batch->count; i++)
        if (batch->chunks[i].cx == source->cx && batch->chunks[i].cz == source->cz) {
            errno = EEXIST;
            return -1;
        }
    mc_save_clock_t span = mc_save_profile_begin();
    size_t bytes = 0;
    int rc = mc_chunk_save_snapshot(source, &batch->chunks[batch->count],
                                    MC_SAVE_SNAPSHOT_BYTES - batch->bytes, &bytes);
    mc_save_profile_end("save_snapshot", source->cx, source->cz, span, bytes, rc);
    if (rc != 0)
        return -1;
    batch->bytes += bytes;
    batch->count++;
    return 0;
}
static int order(const void *av, const void *bv) {
    const mc_chunk_t *a = *(mc_chunk_t *const *)av, *b = *(mc_chunk_t *const *)bv;
    int32_t ax, az, bx, bz;
    unsigned as, bs;
    mc_region_coordinates(a->cx, a->cz, &ax, &az, &as);
    mc_region_coordinates(b->cx, b->cz, &bx, &bz, &bs);
    if (ax != bx)
        return ax < bx ? -1 : 1;
    if (az != bz)
        return az < bz ? -1 : 1;
    return (as > bs) - (as < bs);
}
void mc_save_batch_execute(const mc_world_storage_t *store, mc_save_batch_t *batch) {
    mc_chunk_t *sorted[MC_SAVE_BATCH_CHUNKS];
    for (size_t i = 0; i < batch->count; i++)
        sorted[i] = &batch->chunks[i];
    qsort(sorted, batch->count, sizeof(*sorted), order);
    if(mc_storage_is_sqlite(store)){
        int rc=mc_storage_write_region(store,sorted,batch->count);
        int error=rc?(errno?errno:EIO):0;
        for(size_t i=0;i<batch->count;i++){batch->results[i]=rc;batch->errors[i]=error;}
        return;
    }
    for (size_t begin = 0; begin < batch->count;) {
        int32_t rx, rz;
        mc_region_coordinates(sorted[begin]->cx, sorted[begin]->cz, &rx, &rz, NULL);
        size_t end = begin + 1;
        while (end < batch->count) {
            int32_t x, z;
            mc_region_coordinates(sorted[end]->cx, sorted[end]->cz, &x, &z, NULL);
            if (x != rx || z != rz)
                break;
            end++;
        }
        int rc = mc_storage_write_region(store, &sorted[begin], end - begin);
        int saved = rc ? (errno ? errno : EIO) : 0;
        for (size_t i = begin; i < end; i++) {
            size_t at = (size_t)(sorted[i] - batch->chunks);
            batch->results[at] = rc;
            batch->errors[at] = rc ? saved : 0;
        }
        begin = end;
    }
}
void mc_save_batch_destroy(mc_save_batch_t *batch) {
    if (!batch)
        return;
    for (size_t i = 0; i < batch->count; i++)
        mc_chunk_destroy(&batch->chunks[i]);
    free(batch);
}

struct mc_save_io {
    const mc_world_storage_t *store;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    mc_save_batch_t *batch;
    bool completed, stopping;
};
static void *io_main(void *arg) {
    mc_save_io_t *io = arg;
    pthread_mutex_lock(&io->lock);
    for (;;) {
        while (!io->stopping && (!io->batch || io->completed))
            pthread_cond_wait(&io->changed, &io->lock);
        if (io->stopping && (!io->batch || io->completed))
            break;
        mc_save_batch_t *batch = io->batch;
        pthread_mutex_unlock(&io->lock);
        mc_save_batch_execute(io->store, batch);
        pthread_mutex_lock(&io->lock);
        io->completed = true;
        pthread_cond_broadcast(&io->changed);
    }
    pthread_mutex_unlock(&io->lock);
    return NULL;
}
int mc_save_io_create(const mc_world_storage_t *store, mc_save_io_t **out) {
    if (!out || !mc_storage_is_native(store)) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    mc_save_io_t *io = calloc(1, sizeof(*io));
    if (!io)
        return -1;
    io->store = store;
    int rc = pthread_mutex_init(&io->lock, NULL);
    if (rc) {
        free(io);
        errno = rc;
        return -1;
    }
    rc = pthread_cond_init(&io->changed, NULL);
    if (rc) {
        pthread_mutex_destroy(&io->lock);
        free(io);
        errno = rc;
        return -1;
    }
    rc = pthread_create(&io->thread, NULL, io_main, io);
    if (rc) {
        pthread_cond_destroy(&io->changed);
        pthread_mutex_destroy(&io->lock);
        free(io);
        errno = rc;
        return -1;
    }
    *out = io;
    return 0;
}
int mc_save_io_submit(mc_save_io_t *io, mc_save_batch_t *batch) {
    if (!io || !batch || !batch->count || batch->count > MC_SAVE_BATCH_CHUNKS ||
        batch->bytes > MC_SAVE_SNAPSHOT_BYTES) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&io->lock);
    if (io->stopping || io->batch) {
        pthread_mutex_unlock(&io->lock);
        errno = EAGAIN;
        return -1;
    }
    io->batch = batch;
    io->completed = false;
    pthread_cond_signal(&io->changed);
    pthread_mutex_unlock(&io->lock);
    return 0;
}
bool mc_save_io_busy(mc_save_io_t *io) {
    if (!io)
        return false;
    pthread_mutex_lock(&io->lock);
    bool busy = io->batch != NULL;
    pthread_mutex_unlock(&io->lock);
    return busy;
}
mc_save_batch_t *mc_save_io_take(mc_save_io_t *io, bool wait) {
    if (!io)
        return NULL;
    pthread_mutex_lock(&io->lock);
    while (wait && io->batch && !io->completed)
        pthread_cond_wait(&io->changed, &io->lock);
    mc_save_batch_t *batch = io->completed ? io->batch : NULL;
    if (batch) {
        io->batch = NULL;
        io->completed = false;
        pthread_cond_broadcast(&io->changed);
    }
    pthread_mutex_unlock(&io->lock);
    return batch;
}
int mc_save_io_close(mc_save_io_t *io, mc_save_batch_t **completion) {
    if (!io || !completion) {
        errno = EINVAL;
        return -1;
    }
    *completion = NULL;
    pthread_mutex_lock(&io->lock);
    io->stopping = true;
    pthread_cond_broadcast(&io->changed);
    pthread_mutex_unlock(&io->lock);
    int rc = pthread_join(io->thread, NULL);
    if (rc) {
        errno = rc;
        return -1;
    }
    *completion = io->batch;
    pthread_cond_destroy(&io->changed);
    pthread_mutex_destroy(&io->lock);
    free(io);
    return 0;
}
