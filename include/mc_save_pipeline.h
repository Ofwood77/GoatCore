#ifndef MC_SAVE_PIPELINE_H
#define MC_SAVE_PIPELINE_H
#include "mc_world_storage.h"
/* Opt-in policy, independent of the world's persistent backend identity. */
typedef enum { MC_SAVE_IMMEDIATE = 0, MC_SAVE_BATCHED = 1, MC_SAVE_ASYNC = 2 } mc_save_mode_t;
#define MC_SAVE_BATCH_CHUNKS 64u
#define MC_SAVE_SNAPSHOT_BYTES (64u * 1024u * 1024u)
typedef struct {
    mc_chunk_t chunks[MC_SAVE_BATCH_CHUNKS];
    int results[MC_SAVE_BATCH_CHUNKS], errors[MC_SAVE_BATCH_CHUNKS];
    size_t count, bytes;
} mc_save_batch_t;
const char *mc_save_mode_name(mc_save_mode_t mode);
int mc_save_mode_parse(const char *name, mc_save_mode_t *mode);
/* Single owner until submission. add never retains source pointers. */
int mc_save_batch_add(mc_save_batch_t *batch, const mc_chunk_t *source);
/* Deterministic region/slot ordering; per-region atomicity, not a transaction
 * across regions. Writes and result buffers belong exclusively to the caller. */
void mc_save_batch_execute(const mc_world_storage_t *store, mc_save_batch_t *batch);
void mc_save_batch_destroy(mc_save_batch_t *batch);
typedef struct mc_save_io mc_save_io_t;
/* Exactly one batch slot INCLUDING running/completed work. On successful
 * submit ownership transfers; EAGAIN retains caller ownership. The I/O thread
 * owns no world/chunk pointers. Store must outlive close and all completions. */
int mc_save_io_create(const mc_world_storage_t *store, mc_save_io_t **out);
int mc_save_io_submit(mc_save_io_t *io, mc_save_batch_t *batch);
bool mc_save_io_busy(mc_save_io_t *io);
mc_save_batch_t *mc_save_io_take(mc_save_io_t *io, bool wait);
/* Stops acceptance, finishes accepted work, joins, returns any completion to
 * its owner for acknowledgement. Never silently discards snapshots. */
int mc_save_io_close(mc_save_io_t *io, mc_save_batch_t **completion);
#endif
