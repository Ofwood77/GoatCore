#ifndef MC_BLOCK_STATE_H
#define MC_BLOCK_STATE_H
#include "block_registry.h"
#include <stdbool.h>

/* block_index is an internal sorted descriptor index, never a wire block ID. */
const mc_block_desc_t *mc_block_desc_by_name(const char *name);
const mc_block_desc_t *mc_block_state_desc(int32_t state);
bool mc_block_state_property(int32_t state, const char *name, char *value, size_t capacity);
/* Preserves other properties and rejects values absent from the exact registry. */
int32_t mc_block_state_with_property(int32_t state, const char *name, const char *value);
#endif
