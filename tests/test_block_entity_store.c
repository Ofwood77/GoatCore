#include "block_entity_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char *msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
}

static mc_block_entity_t make_entity(mc_block_entity_type_t type, uint32_t a, uint32_t b) {
    mc_block_entity_t entity;
    memset(&entity, 0, sizeof(entity));
    entity.type = type;
    if (type == MC_BLOCK_ENTITY_CHEST) {
        entity.data.container.furnace_burn_time = (int32_t)a;
        entity.data.container.slot_count = b;
    } else if (type == MC_BLOCK_ENTITY_SIGN) {
        entity.data.sign.text_ref = a;
        entity.data.sign.line_count = b;
    } else {
        entity.data.generic.payload_ref = a;
        entity.data.generic.payload_size = b;
    }
    return entity;
}

static int test_basic_crud(void) {
    mc_block_entity_store_t store;
    mc_pos_t chest_pos = {12, 64, -4};
    mc_pos_t sign_pos = {-30, 70, 99};
    mc_block_entity_t *found;

    mc_be_store_init(&store);

    if (!mc_be_store_put(&store, chest_pos, make_entity(MC_BLOCK_ENTITY_CHEST, 10, 27))) {
        mc_be_store_destroy(&store);
        return fail("put chest");
    }
    if (!mc_be_store_put(&store, sign_pos, make_entity(MC_BLOCK_ENTITY_SIGN, 77, 4))) {
        mc_be_store_destroy(&store);
        return fail("put sign");
    }

    found = mc_be_store_get(&store, chest_pos);
    if (!found || found->type != MC_BLOCK_ENTITY_CHEST || found->data.container.slot_count != 27) {
        mc_be_store_destroy(&store);
        return fail("get chest");
    }

    found = mc_be_store_get(&store, sign_pos);
    if (!found || found->type != MC_BLOCK_ENTITY_SIGN || found->data.sign.line_count != 4) {
        mc_be_store_destroy(&store);
        return fail("get sign");
    }

    if (!mc_be_store_remove(&store, chest_pos)) {
        mc_be_store_destroy(&store);
        return fail("remove chest");
    }
    if (mc_be_store_get(&store, chest_pos) != NULL) {
        mc_be_store_destroy(&store);
        return fail("removed entity still visible");
    }

    mc_be_store_destroy(&store);
    return 0;
}

static int test_update_existing(void) {
    mc_block_entity_store_t store;
    mc_pos_t pos = {1, 65, 1};
    mc_block_entity_t *found;

    mc_be_store_init(&store);
    if (!mc_be_store_put(&store, pos, make_entity(MC_BLOCK_ENTITY_CHEST, 1, 27))) {
        mc_be_store_destroy(&store);
        return fail("put initial");
    }
    if (!mc_be_store_put(&store, pos, make_entity(MC_BLOCK_ENTITY_CHEST, 2, 54))) {
        mc_be_store_destroy(&store);
        return fail("update existing");
    }

    found = mc_be_store_get(&store, pos);
    if (!found || found->data.container.furnace_burn_time != 2 || found->data.container.slot_count != 54) {
        mc_be_store_destroy(&store);
        return fail("updated value mismatch");
    }
    if (store.len != 1) {
        mc_be_store_destroy(&store);
        return fail("update changed len");
    }

    mc_be_store_destroy(&store);
    return 0;
}

static int test_growth_and_collisions(void) {
    mc_block_entity_store_t store;
    mc_pos_t positions[256];

    mc_be_store_init(&store);

    for (size_t i = 0; i < 256; i++) {
        positions[i].x = (int32_t)((i * 37u) % 97u) - 48;
        positions[i].y = (int32_t)(32 + ((i * 13u) % 80u));
        positions[i].z = (int32_t)((i * 53u) % 101u) - 50;

        if (!mc_be_store_put(&store, positions[i], make_entity(MC_BLOCK_ENTITY_GENERIC, (uint32_t)i, (uint32_t)(i + 1000)))) {
            mc_be_store_destroy(&store);
            return fail("bulk put");
        }
    }

    if (store.cap < 256) {
        mc_be_store_destroy(&store);
        return fail("store did not grow");
    }

    for (size_t i = 0; i < 256; i++) {
        mc_block_entity_t *found = mc_be_store_get(&store, positions[i]);
        if (!found || found->type != MC_BLOCK_ENTITY_GENERIC || found->data.generic.payload_ref != i) {
            mc_be_store_destroy(&store);
            return fail("bulk get");
        }
    }

    for (size_t i = 0; i < 128; i++) {
        if (!mc_be_store_remove(&store, positions[i])) {
            mc_be_store_destroy(&store);
            return fail("bulk remove");
        }
    }

    for (size_t i = 128; i < 256; i++) {
        mc_block_entity_t *found = mc_be_store_get(&store, positions[i]);
        if (!found || found->data.generic.payload_size != i + 1000) {
            mc_be_store_destroy(&store);
            return fail("post-remove get");
        }
    }

    mc_be_store_destroy(&store);
    return 0;
}

int main(void) {
    if (test_basic_crud() != 0) return 1;
    if (test_update_existing() != 0) return 1;
    if (test_growth_and_collisions() != 0) return 1;
    return 0;
}
