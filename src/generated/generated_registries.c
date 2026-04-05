#include "generated_registries.h"

/* BlockState lookup compatibility shim.
 * The authoritative BlockState registry is generated in block_registry.c. */

int mc_block_state_id(const char *name, int fallback) {
    return (int)mc_global_state_id_from_key(name, (mc_global_state_id_t)fallback);
}

const char *mc_block_state_key(int id) {
    if (id < 0) return NULL;
    return mc_global_state_key((mc_global_state_id_t)id);
}
