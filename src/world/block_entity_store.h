#ifndef MC_BLOCK_ENTITY_STORE_H
#define MC_BLOCK_ENTITY_STORE_H

#include "mc_inventory.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int32_t x;
    int32_t y;
    int32_t z;
} mc_pos_t;

typedef enum {
    MC_BLOCK_ENTITY_NONE = 0,
    MC_BLOCK_ENTITY_CHEST = 1,
    MC_BLOCK_ENTITY_BARREL = 2,
    MC_BLOCK_ENTITY_DROPPER = 3,
    MC_BLOCK_ENTITY_SHULKER_BOX = 4,
    MC_BLOCK_ENTITY_ENDER_CHEST = 5,
    MC_BLOCK_ENTITY_SIGN = 6,
    MC_BLOCK_ENTITY_GENERIC = 255
} mc_block_entity_type_t;

typedef struct {
    mc_block_entity_type_t type;
    uint16_t flags;
    union {
        struct {
            uint32_t slot_count;
            mc_slot_t slots[MC_CONTAINER_SLOT_COUNT];
        } container;

        struct {
            uint32_t text_ref;
            uint32_t line_count;
        } sign;

        struct {
            uint32_t payload_ref;
            uint32_t payload_size;
        } generic;
    } data;
} mc_block_entity_t;

typedef struct {
    mc_pos_t *positions;
    mc_block_entity_t *entities;
    uint8_t *states;
    size_t cap;
    size_t len;
    size_t tombs;
} mc_block_entity_store_t;

void mc_be_store_init(mc_block_entity_store_t *store);
void mc_be_store_destroy(mc_block_entity_store_t *store);
bool mc_be_store_put(mc_block_entity_store_t *store, mc_pos_t pos, mc_block_entity_t entity);
mc_block_entity_t *mc_be_store_get(mc_block_entity_store_t *store, mc_pos_t pos);
bool mc_be_store_remove(mc_block_entity_store_t *store, mc_pos_t pos);

#endif /* MC_BLOCK_ENTITY_STORE_H */
