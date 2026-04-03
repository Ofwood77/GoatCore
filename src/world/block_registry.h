#ifndef MC_BLOCK_REGISTRY_H
#define MC_BLOCK_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t mc_global_state_id_t;
typedef uint32_t mc_block_id_t;

enum {
    MC_BLOCK_FLAG_VALID = 1u << 0,
    MC_BLOCK_FLAG_IS_AIR = 1u << 1,
    MC_BLOCK_FLAG_IS_DEFAULT_STATE = 1u << 2,
    MC_BLOCK_FLAG_IS_SOLID = 1u << 3,
    MC_BLOCK_FLAG_IS_OPAQUE = 1u << 4,
    MC_BLOCK_FLAG_HAS_BLOCK_ENTITY = 1u << 5
};

typedef struct {
    const char *name;
    mc_global_state_id_t default_state;
    mc_global_state_id_t min_state_id;
    mc_global_state_id_t max_state_id;
} mc_block_desc_t;

typedef struct {
    uint32_t flags;
    uint8_t luminance;
    uint8_t reserved0;
    uint16_t block_index;
} mc_block_properties_t;

extern const mc_block_desc_t GLOBAL_BLOCKS[];
extern const size_t GLOBAL_BLOCK_COUNT;
extern const mc_block_properties_t GLOBAL_BLOCK_STATES[];
extern const size_t GLOBAL_BLOCK_STATES_COUNT;

mc_global_state_id_t mc_global_state_id_from_key(const char *key, mc_global_state_id_t fallback);
const char *mc_global_state_key(mc_global_state_id_t id);

#endif /* MC_BLOCK_REGISTRY_H */
