#ifndef GENERATED_REGISTRIES_H
#define GENERATED_REGISTRIES_H

#include "block_registry.h"

#define MC_BLOCK_STATE_MAX_ID ((int)(GLOBAL_BLOCK_STATES_COUNT - 1u))

int mc_block_state_id(const char *name, int fallback);
const char *mc_block_state_key(int id);

#endif /* GENERATED_REGISTRIES_H */
