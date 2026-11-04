#ifndef MC_CHUNK_MIGRATION_H
#define MC_CHUNK_MIGRATION_H
#include <stddef.h>
#include <stdint.h>

/* Offline tool only, deliberately absent from mc_server. No source writes.
 * A supported BIN is exact NBT, 4785/4788, full Overworld, canonical palettes.
 * Only DataVersion and block-state long packing change. Unknown NBT stays exact.
 * Reject rather than approximate unsupported inventories/biomes/generation. */
#define MC_MIGRATION_MAX_NBT (8u * 1024u * 1024u)
typedef struct {
    int source_version;
    unsigned compact_sections, padded_sections, block_entities;
} mc_chunk_migration_info_t;
int mc_chunk_migrate_nbt(const uint8_t *raw, size_t length, int32_t cx, int32_t cz, uint8_t **output,
                         size_t *output_length, mc_chunk_migration_info_t *info, char reason[160]);
#endif
