#ifndef MC_REGION_STORAGE_H
#define MC_REGION_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MC_REGION_SLOTS 1024u
#define MC_REGION_SECTOR_BYTES 4096u
#define MC_REGION_HEADER_BYTES 8192u
#define MC_REGION_MAX_OFFSET 0xFFFFFFu
#define MC_REGION_MAX_SECTORS (MC_REGION_MAX_OFFSET + 255u)

typedef struct mc_region_index mc_region_index_t;
typedef struct mc_region_file mc_region_file_t;
typedef struct mc_region_plan mc_region_plan_t;

typedef struct {
    uint32_t offset, length, timestamp; /* length includes compression byte, excludes its 4-byte prefix */
    uint8_t sectors, compression;       /* compression retains external flag */
    bool external, decodable;
} mc_region_entry_t;

typedef struct {
    uint64_t file_bytes;
    uint32_t sectors, occupied, free_sectors, chunks, external, unsupported;
    size_t metadata_bytes; /* index + bitmap, excludes allocator overhead */
} mc_region_stats_t;

typedef struct {
    uint64_t device, inode, file_bytes;
    int64_t mtime_seconds, mtime_nanoseconds;
    uint32_t header_crc32; /* diagnostic identity, NOT a transaction generation or collision-proof token */
} mc_region_source_t;

typedef struct {
    uint32_t offset, sectors;
    uint64_t ticket;
} mc_region_reservation_t;
typedef struct {
    unsigned slot;
    uint32_t offset, sectors;
    uint64_t epoch;
} mc_region_retirement_t;

/* Read-only immutable snapshot; fd is borrowed. Reject malformed tables and
 * changing in-place files. Unknown codecs and .mcc stubs remain inspectable.
 * Readers/writers still require exclusive world ownership or cooperation. */
int mc_region_index_load(int fd, mc_region_index_t **out);
void mc_region_index_free(mc_region_index_t *index);
const mc_region_entry_t *mc_region_entry(const mc_region_index_t *index, unsigned slot);
const mc_region_stats_t *mc_region_stats(const mc_region_index_t *index);
const mc_region_source_t *mc_region_source(const mc_region_index_t *index);
const uint8_t *mc_region_header(const mc_region_index_t *index);
bool mc_region_sector_used(const mc_region_index_t *index, uint32_t sector);
void mc_region_coordinates(int32_t cx, int32_t cz, int32_t *rx, int32_t *rz, unsigned *slot);

/* Pinned read-only inode: rename of pathname does not switch an open reader.
 * Open returns 1 for ENOENT; record returns 1 for absent slot. Other errors -1.
 * Record is malloc'ed, includes length + codec + opaque payload, never parses NBT.
 * No .mcc file is opened or modified. No locks or files are created by readers. */
int mc_region_open(const char *path, mc_region_file_t **out);
int mc_region_close(mc_region_file_t *file);
const mc_region_index_t *mc_region_file_index(const mc_region_file_t *file);
int mc_region_read_record(mc_region_file_t *file, unsigned slot, uint8_t **out, size_t *length);

/* S1 allocation simulation ONLY: never writes, truncates, syncs or publishes.
 * Single-owner mutable plan, immutable source copied. NULL creates a virtual
 * empty two-sector region. Unknown codecs/.mcc fail closed (EOPNOTSUPP).
 * Recovered journal pins MUST be installed before the first reservation.
 * Retirement records never free live sectors; S1 exposes no reclaim/commit API.
 * S2 must durably journal generations, validate source, sync payload, publish
 * recoverable metadata, then retire only after all recovery references expire. */
int mc_region_plan_create(const mc_region_index_t *index, mc_region_plan_t **out);
void mc_region_plan_free(mc_region_plan_t *plan);
int mc_region_plan_protect(mc_region_plan_t *plan, uint32_t offset, uint32_t count);
int mc_region_plan_reserve(mc_region_plan_t *plan, uint32_t count, mc_region_reservation_t *out);
int mc_region_plan_reserve_at(mc_region_plan_t *plan, uint32_t offset, uint32_t count,
                              mc_region_reservation_t *out);
int mc_region_plan_cancel(mc_region_plan_t *plan, mc_region_reservation_t reservation);
int mc_region_plan_retire(mc_region_plan_t *plan, unsigned slot, uint64_t epoch);
const mc_region_retirement_t *mc_region_plan_retirement(const mc_region_plan_t *plan, unsigned slot);
const mc_region_source_t *mc_region_plan_source(const mc_region_plan_t *plan);
uint32_t mc_region_plan_sectors(const mc_region_plan_t *plan);
bool mc_region_plan_used(const mc_region_plan_t *plan, uint32_t sector);

#endif
