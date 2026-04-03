#include "paletted_container.h"

#include <stdlib.h>
#include <string.h>

#define MC_PALETTE_BITS 4u
#define MC_DIRECT_BITS 15u
#define MC_PALETTE_CAPACITY 16u

static bool state_is_air(mc_global_state_id_t id) {
    if ((size_t)id >= GLOBAL_BLOCK_STATES_COUNT) return false;
    return (GLOBAL_BLOCK_STATES[id].flags & MC_BLOCK_FLAG_IS_AIR) != 0u;
}

static bool state_is_valid(mc_global_state_id_t id) {
    if (id > 0x7FFFu) return false;
    if ((size_t)id >= GLOBAL_BLOCK_STATES_COUNT) return true;
    return (GLOBAL_BLOCK_STATES[id].flags & MC_BLOCK_FLAG_VALID) != 0u;
}

static bool coords_valid(int x, int y, int z) {
    return x >= 0 && x < MC_SECTION_EDGE &&
           y >= 0 && y < MC_SECTION_EDGE &&
           z >= 0 && z < MC_SECTION_EDGE;
}

static size_t section_index(int x, int y, int z) {
    return (size_t)(((y << 4) | z) << 4 | x);
}

static size_t section_word_count_for_bits(uint8_t bits) {
    if (bits == 0) return 0;
    return ((size_t)MC_SECTION_VOLUME * (size_t)bits + 63u) / 64u;
}

static uint32_t unpack_value(const uint64_t *words, size_t index, uint8_t bits) {
    size_t bit_index;
    size_t word_index;
    uint64_t value;
    uint64_t mask;
    int shift;
    int spill;

    if (!words || bits == 0) return 0;

    bit_index = index * (size_t)bits;
    word_index = bit_index >> 6;
    shift = (int)(bit_index & 63u);
    value = words[word_index] >> shift;
    spill = shift + (int)bits - 64;
    if (spill > 0) {
        value |= words[word_index + 1] << (bits - (uint8_t)spill);
    }
    mask = (1ULL << bits) - 1ULL;
    return (uint32_t)(value & mask);
}

static void pack_value(uint64_t *words, size_t index, uint8_t bits, uint32_t value) {
    size_t bit_index;
    size_t word_index;
    uint64_t mask;
    uint64_t v;
    int shift;
    int spill;

    if (!words || bits == 0) return;

    bit_index = index * (size_t)bits;
    word_index = bit_index >> 6;
    shift = (int)(bit_index & 63u);
    mask = (1ULL << bits) - 1ULL;
    v = (uint64_t)value & mask;

    words[word_index] &= ~(mask << shift);
    words[word_index] |= v << shift;

    spill = shift + (int)bits - 64;
    if (spill > 0) {
        uint64_t hi_mask = (1ULL << spill) - 1ULL;
        words[word_index + 1] &= ~hi_mask;
        words[word_index + 1] |= v >> (bits - (uint8_t)spill);
    }
}

static int palette_find_local_id(const mc_paletted_container_t *sec, mc_global_state_id_t global_id) {
    uint16_t i;

    if (!sec) return -1;
    for (i = 0; i < sec->palette_len; i++) {
        if (sec->palette[i] == global_id) return (int)i;
    }
    return -1;
}

static int promote_uniform_to_palette4(mc_paletted_container_t *sec, mc_global_state_id_t new_state) {
    uint64_t *words;
    size_t word_count;
    uint32_t fill_local;
    size_t i;

    if (!sec) return -1;

    word_count = section_word_count_for_bits(MC_PALETTE_BITS);
    words = (uint64_t *)calloc(word_count, sizeof(*words));
    if (!words) return -1;

    sec->palette[0] = sec->uniform_state;
    sec->palette[1] = new_state;
    sec->palette_len = 2;
    sec->bits_per_block = MC_PALETTE_BITS;
    sec->is_direct = 0;
    sec->words = words;
    sec->word_count = word_count;

    fill_local = 0u;
    for (i = 0; i < MC_SECTION_VOLUME; i++) {
        pack_value(sec->words, i, sec->bits_per_block, fill_local);
    }
    return 0;
}

static int promote_palette4_to_direct15(mc_paletted_container_t *sec) {
    uint64_t *words;
    size_t word_count;
    size_t i;

    if (!sec) return -1;

    word_count = section_word_count_for_bits(MC_DIRECT_BITS);
    words = (uint64_t *)calloc(word_count, sizeof(*words));
    if (!words) return -1;

    for (i = 0; i < MC_SECTION_VOLUME; i++) {
        uint32_t local_id = unpack_value(sec->words, i, sec->bits_per_block);
        mc_global_state_id_t state = sec->palette[local_id];
        pack_value(words, i, MC_DIRECT_BITS, state);
    }

    free(sec->words);
    sec->words = words;
    sec->word_count = word_count;
    sec->bits_per_block = MC_DIRECT_BITS;
    sec->is_direct = 1;
    sec->palette_len = 0;
    memset(sec->palette, 0, sizeof(sec->palette));
    return 0;
}

int mc_paletted_container_init(mc_paletted_container_t *sec, mc_global_state_id_t fill_state) {
    if (!sec) return -1;
    if (!state_is_valid(fill_state)) return -1;

    memset(sec, 0, sizeof(*sec));
    sec->uniform_state = fill_state;
    sec->non_air_blocks_count = state_is_air(fill_state) ? 0u : (uint16_t)MC_SECTION_VOLUME;
    return 0;
}

void mc_paletted_container_destroy(mc_paletted_container_t *sec) {
    if (!sec) return;
    free(sec->words);
    memset(sec, 0, sizeof(*sec));
}

mc_global_state_id_t mc_paletted_container_get_block(const mc_paletted_container_t *sec, int x, int y, int z) {
    size_t index;
    uint32_t value;

    if (!sec) return 0;
    if (!coords_valid(x, y, z)) {
        return sec->bits_per_block == 0 ? sec->uniform_state : 0;
    }
    if (sec->bits_per_block == 0) return sec->uniform_state;

    index = section_index(x, y, z);
    value = unpack_value(sec->words, index, sec->bits_per_block);
    if (sec->is_direct) return (mc_global_state_id_t)value;
    if (value >= sec->palette_len) return 0;
    return sec->palette[value];
}

int mc_paletted_container_set_block(mc_paletted_container_t *sec, int x, int y, int z, mc_global_state_id_t global_id) {
    mc_global_state_id_t old_id;
    bool old_is_air;
    bool new_is_air;
    size_t index;
    int local_id;

    if (!sec || !coords_valid(x, y, z)) return -1;
    if (!state_is_valid(global_id)) return -1;
    if (global_id > 0x7FFFu) return -1;

    old_id = mc_paletted_container_get_block(sec, x, y, z);
    if (old_id == global_id) return 0;

    old_is_air = state_is_air(old_id);
    new_is_air = state_is_air(global_id);
    index = section_index(x, y, z);

    if (sec->bits_per_block == 0) {
        if (promote_uniform_to_palette4(sec, global_id) != 0) return -1;
    } else if (!sec->is_direct && sec->bits_per_block == MC_PALETTE_BITS) {
        local_id = palette_find_local_id(sec, global_id);
        if (local_id < 0) {
            if (sec->palette_len < MC_PALETTE_CAPACITY) {
                sec->palette[sec->palette_len] = global_id;
                local_id = (int)sec->palette_len;
                sec->palette_len++;
            } else {
                if (promote_palette4_to_direct15(sec) != 0) return -1;
            }
        }
    }

    if (sec->bits_per_block == 0) {
        sec->uniform_state = global_id;
    } else if (sec->is_direct) {
        pack_value(sec->words, index, sec->bits_per_block, global_id);
    } else {
        local_id = palette_find_local_id(sec, global_id);
        if (local_id < 0) return -1;
        pack_value(sec->words, index, sec->bits_per_block, (uint32_t)local_id);
    }

    if (old_is_air != new_is_air) {
        if (new_is_air) sec->non_air_blocks_count--;
        else sec->non_air_blocks_count++;
    }
    return 0;
}

uint8_t mc_paletted_container_bits_per_block(const mc_paletted_container_t *sec) {
    return sec ? sec->bits_per_block : 0u;
}

uint16_t mc_paletted_container_non_air_count(const mc_paletted_container_t *sec) {
    return sec ? sec->non_air_blocks_count : 0u;
}

uint16_t mc_paletted_container_palette_len(const mc_paletted_container_t *sec) {
    return sec ? sec->palette_len : 0u;
}

bool mc_paletted_container_is_direct(const mc_paletted_container_t *sec) {
    return sec ? sec->is_direct != 0 : false;
}
