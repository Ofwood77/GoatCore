#include "block_entity_store.h"

#include <stdlib.h>
#include <string.h>

static void block_entity_clear(mc_block_entity_t *entity) {
    if (!entity) return;
    if (entity->type == MC_BLOCK_ENTITY_CHEST || entity->type == MC_BLOCK_ENTITY_BARREL || entity->type == MC_BLOCK_ENTITY_DROPPER ||
        entity->type == MC_BLOCK_ENTITY_SHULKER_BOX || entity->type == MC_BLOCK_ENTITY_ENDER_CHEST) {
        uint32_t slot_count = entity->data.container.slot_count;
        if (slot_count > MC_CONTAINER_SLOT_COUNT) slot_count = MC_CONTAINER_SLOT_COUNT;
        for (uint32_t i = 0; i < slot_count; i++) mc_slot_clear(&entity->data.container.slots[i]);
    }
    memset(entity, 0, sizeof(*entity));
}

enum {
    MC_BE_SLOT_EMPTY = 0,
    MC_BE_SLOT_FILLED = 1,
    MC_BE_SLOT_TOMB = 2,
    MC_BE_STORE_MIN_CAP = 16
};

static bool pos_equal(mc_pos_t a, mc_pos_t b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

static uint64_t mix_u64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static uint64_t hash_pos(mc_pos_t pos) {
    uint64_t x = (uint32_t)pos.x;
    uint64_t y = (uint32_t)pos.y;
    uint64_t z = (uint32_t)pos.z;
    return mix_u64(x ^ (y << 21) ^ (z << 42) ^ (y >> 11) ^ (z >> 7));
}

static size_t next_pow2(size_t v) {
    size_t x = 1;
    while (x < v) x <<= 1;
    return x;
}

static bool store_allocate(mc_block_entity_store_t *store, size_t cap) {
    if (!store) return false;
    if (cap < MC_BE_STORE_MIN_CAP) cap = MC_BE_STORE_MIN_CAP;
    cap = next_pow2(cap);

    store->positions = (mc_pos_t *)calloc(cap, sizeof(*store->positions));
    store->entities = (mc_block_entity_t *)calloc(cap, sizeof(*store->entities));
    store->states = (uint8_t *)calloc(cap, sizeof(*store->states));
    if (!store->positions || !store->entities || !store->states) {
        free(store->positions);
        free(store->entities);
        free(store->states);
        store->positions = NULL;
        store->entities = NULL;
        store->states = NULL;
        return false;
    }

    store->cap = cap;
    store->len = 0;
    store->tombs = 0;
    return true;
}

static bool store_rehash(mc_block_entity_store_t *store, size_t new_cap) {
    mc_block_entity_store_t next;

    mc_be_store_init(&next);
    if (!store_allocate(&next, new_cap)) return false;

    for (size_t i = 0; i < store->cap; i++) {
        if (store->states[i] != MC_BE_SLOT_FILLED) continue;
        if (!mc_be_store_put(&next, store->positions[i], store->entities[i])) {
            mc_be_store_destroy(&next);
            return false;
        }
    }

    free(store->positions);
    free(store->entities);
    free(store->states);
    *store = next;
    return true;
}

static bool store_maybe_grow(mc_block_entity_store_t *store) {
    size_t used;

    if (!store) return false;
    if (store->cap == 0) return store_allocate(store, MC_BE_STORE_MIN_CAP);

    used = store->len + store->tombs;
    if (used * 10 < store->cap * 7) return true;
    return store_rehash(store, store->cap * 2);
}

static ptrdiff_t store_find_index(const mc_block_entity_store_t *store, mc_pos_t pos, bool for_insert, size_t *out_tomb) {
    size_t mask;
    size_t idx;
    size_t first_tomb = (size_t)-1;

    if (out_tomb) *out_tomb = (size_t)-1;
    if (!store || store->cap == 0 || !store->states) return -1;

    mask = store->cap - 1;
    idx = (size_t)hash_pos(pos) & mask;

    for (;;) {
        uint8_t state = store->states[idx];
        if (state == MC_BE_SLOT_EMPTY) {
            if (out_tomb) *out_tomb = first_tomb;
            return for_insert ? (ptrdiff_t)idx : -1;
        }
        if (state == MC_BE_SLOT_TOMB) {
            if (for_insert && first_tomb == (size_t)-1) first_tomb = idx;
        } else if (pos_equal(store->positions[idx], pos)) {
            if (out_tomb) *out_tomb = first_tomb;
            return (ptrdiff_t)idx;
        }
        idx = (idx + 1) & mask;
    }
}

void mc_be_store_init(mc_block_entity_store_t *store) {
    if (!store) return;
    memset(store, 0, sizeof(*store));
}

void mc_be_store_destroy(mc_block_entity_store_t *store) {
    if (!store) return;
    for (size_t i = 0; i < store->cap; i++) {
        if (store->states && store->states[i] == MC_BE_SLOT_FILLED) block_entity_clear(&store->entities[i]);
    }
    free(store->positions);
    free(store->entities);
    free(store->states);
    memset(store, 0, sizeof(*store));
}

bool mc_be_store_put(mc_block_entity_store_t *store, mc_pos_t pos, mc_block_entity_t entity) {
    size_t tomb = (size_t)-1;
    ptrdiff_t found;
    size_t use_idx;

    if (!store_maybe_grow(store)) return false;

    found = store_find_index(store, pos, true, &tomb);
    if (found < 0) return false;

    if (store->states[(size_t)found] == MC_BE_SLOT_FILLED) {
        block_entity_clear(&store->entities[(size_t)found]);
        store->entities[(size_t)found] = entity;
        return true;
    }

    use_idx = (tomb != (size_t)-1) ? tomb : (size_t)found;
    if (tomb != (size_t)-1) store->tombs--;

    store->states[use_idx] = MC_BE_SLOT_FILLED;
    store->positions[use_idx] = pos;
    store->entities[use_idx] = entity;
    store->len++;
    return true;
}

mc_block_entity_t *mc_be_store_get(mc_block_entity_store_t *store, mc_pos_t pos) {
    ptrdiff_t idx = store_find_index(store, pos, false, NULL);
    if (idx < 0) return NULL;
    if (store->states[(size_t)idx] != MC_BE_SLOT_FILLED) return NULL;
    return &store->entities[(size_t)idx];
}

bool mc_be_store_remove(mc_block_entity_store_t *store, mc_pos_t pos) {
    ptrdiff_t idx = store_find_index(store, pos, false, NULL);
    if (idx < 0) return false;
    if (store->states[(size_t)idx] != MC_BE_SLOT_FILLED) return false;

    block_entity_clear(&store->entities[(size_t)idx]);
    store->states[(size_t)idx] = MC_BE_SLOT_TOMB;
    store->len--;
    store->tombs++;
    return true;
}
