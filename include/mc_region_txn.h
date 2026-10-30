#ifndef MC_REGION_TXN_H
#define MC_REGION_TXN_H

#include "mc_region_storage.h"
#include <stddef.h>
#include <stdint.h>

/* This module is deliberately NOT linked into mc_server. Test tools must opt
 * in explicitly AND own a directory carrying this exact disposable marker. */
#define MC_REGION_TXN_DISPOSABLE 0x53325453u
#define MC_REGION_TXN_MARKER ".goatcore-s2-disposable"
#define MC_REGION_TXN_MARKER_TEXT "GoatCore S2 disposable tests only\n"
#define MC_REGION_TXN_RECOVERY_REQUIRED (-2)

typedef struct mc_region_txn_store mc_region_txn_store_t;
typedef struct {
    unsigned slot;
    const uint8_t *nbt;
    size_t length;
} mc_region_update_t;

/* Holds an exclusive nonblocking directory owner lock until close. All pending
 * canonical journals are recovered BEFORE returning a usable handle. Unknown,
 * stale or corrupt journals fail closed, never delete/regenerate their data.
 * One owner thread per handle; no thread or simulation worker is created. */
int mc_region_txn_open(const char *directory, unsigned explicit_opt_in, mc_region_txn_store_t **out);
int mc_region_txn_close(mc_region_txn_store_t *store);

/* One atomic logical batch (undo before commit, verified redo after commit) sorted by slot.
 * Duplicates/deletes, external/unsupported codecs, >255 sectors/chunk, >64 MiB compressed batch are rejected.
 * Input NBT is immutable and opaque, compressed as zlib. 0 = durable acknowledgement.
 * -1 means no existing chunk changed (empty bootstrap files may remain);
 * -2 is NOT acknowledged and requires close+open/recovery. A failed attempted save poisons
 * the session; subsequent writes require close+open. Pure preflight rejection leaves it untouched.
 * There is no transaction across different regions or files such as players.
 * Only the experimental store coordinates its reads/writes; ordinary
 * Vanilla and S1 readers MUST NOT concurrently access an in-progress transaction. */
int mc_region_txn_write(mc_region_txn_store_t *store, int32_t rx, int32_t rz,
                        const mc_region_update_t *updates, size_t count);

#endif
