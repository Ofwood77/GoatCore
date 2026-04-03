#ifndef MC_PALETTED_CONTAINER_H
#define MC_PALETTED_CONTAINER_H

#include "block_registry.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MC_SECTION_EDGE 16
#define MC_SECTION_VOLUME 4096

typedef struct mc_paletted_container {
    mc_global_state_id_t uniform_state;
    mc_global_state_id_t palette[16];
    uint16_t palette_len;
    uint8_t bits_per_block;
    uint8_t is_direct;
    uint16_t non_air_blocks_count;
    uint64_t *words;
    size_t word_count;
} mc_paletted_container_t;

int mc_paletted_container_init(mc_paletted_container_t *sec, mc_global_state_id_t fill_state);
void mc_paletted_container_destroy(mc_paletted_container_t *sec);

mc_global_state_id_t mc_paletted_container_get_block(const mc_paletted_container_t *sec, int x, int y, int z);
int mc_paletted_container_set_block(mc_paletted_container_t *sec, int x, int y, int z, mc_global_state_id_t global_id);

uint8_t mc_paletted_container_bits_per_block(const mc_paletted_container_t *sec);
uint16_t mc_paletted_container_non_air_count(const mc_paletted_container_t *sec);
uint16_t mc_paletted_container_palette_len(const mc_paletted_container_t *sec);
bool mc_paletted_container_is_direct(const mc_paletted_container_t *sec);

#endif /* MC_PALETTED_CONTAINER_H */
