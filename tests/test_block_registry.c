#include "block_registry.h"
#include "mc_block_state.h"
#include "mc_placement.h"
#include "generated_block_ids.h"
#include <math.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static mc_global_state_id_t expect_roundtrip(const char *key) {
    mc_global_state_id_t id = mc_global_state_id_from_key(key, UINT32_MAX);
    assert(id != UINT32_MAX);
    assert(mc_global_state_key(id) != NULL);
    assert(strcmp(mc_global_state_key(id), key) == 0);
    return id;
}

int main(void) {
    assert(GLOBAL_BLOCK_COUNT == 1168u);
    assert(GLOBAL_BLOCK_STATES_COUNT == 29873u);
    size_t defaults = 0;
    bool protocol_ids[1168] = {false};
    for (uint32_t i = 0; i < GLOBAL_BLOCK_STATES_COUNT; i++) {
        assert(expect_roundtrip(mc_global_state_key(i)) == i);
        const mc_block_desc_t *desc = mc_block_state_desc((int32_t)i);
        assert(desc && i >= desc->min_state_id && i <= desc->max_state_id);
        assert(mc_block_desc_by_name(desc->name) == desc);
        const char *p = strchr(mc_global_state_key(i), '[');
        while (p && *p != ']') {
            p++;
            const char *eq = strchr(p, '=');
            assert(eq);
            char name[64], value[64], actual[64];
            size_t n = (size_t)(eq - p), v = strcspn(eq + 1, ",]");
            assert(n < sizeof(name) && v < sizeof(value));
            memcpy(name, p, n); name[n] = 0;
            memcpy(value, eq + 1, v); value[v] = 0;
            assert(mc_block_state_property((int32_t)i, name, actual, sizeof(actual)));
            assert(strcmp(actual, value) == 0);
            assert(mc_block_state_with_property((int32_t)i, name, actual) == (int32_t)i);
            p = eq + 1 + v;
        }
        assert(GLOBAL_BLOCK_STATES[desc->default_state].flags & MC_BLOCK_FLAG_IS_DEFAULT_STATE);
        if (GLOBAL_BLOCK_STATES[i].flags & MC_BLOCK_FLAG_IS_DEFAULT_STATE) {
            int32_t protocol_id = mc_block_protocol_id_for_state((int32_t)i);
            assert(protocol_id >= 0 && protocol_id < 1168 && !protocol_ids[protocol_id]);
            protocol_ids[protocol_id] = true;
            defaults++;
        }
    }
    assert(defaults == GLOBAL_BLOCK_COUNT);
    for (int i = 0; i < 1168; i++) assert(protocol_ids[i]);
    assert(mc_block_protocol_id_for_state(-1) == -1);
    assert(mc_block_protocol_id_for_state(29873) == -1);
    assert(mc_block_protocol_id_for_state(expect_roundtrip("minecraft:chest[facing=north,type=single,waterlogged=false]")) == 201);
    assert(mc_block_desc_by_name("minecraft:missing") == NULL);
    assert(mc_block_desc_by_name(NULL) == NULL);
    assert(mc_block_state_desc(-1) == NULL);
    assert(mc_block_state_desc(29873) == NULL);
    int32_t log = expect_roundtrip("minecraft:oak_log[axis=y]");
    char value[16];
    assert(mc_block_state_property(log, "axis", value, sizeof(value)) && !strcmp(value, "y"));
    assert(!mc_block_state_property(log, "xis", value, sizeof(value)));
    assert(!mc_block_state_property(log, "axis", value, 1));
    assert(mc_block_state_with_property(log, "axis", "w") == -1);
    assert(mc_block_state_with_property(log, "axis", "x,axis=y") == -1);
    for (int face = 0; face < 6; face++) {
        int32_t next = mc_placement_resolve_simple(log, face, 0.5f);
        assert(mc_block_state_property(next, "axis", value, sizeof(value)));
        assert(!strcmp(value, face < 2 ? "y" : face < 4 ? "z" : "x"));
    }
    int32_t bottom = expect_roundtrip("minecraft:oak_slab[type=bottom,waterlogged=false]");
    int32_t top = expect_roundtrip("minecraft:oak_slab[type=top,waterlogged=false]");
    int32_t full = expect_roundtrip("minecraft:oak_slab[type=double,waterlogged=false]");
    int32_t wet = mc_block_state_with_property(bottom, "waterlogged", "true");
    assert(wet >= 0 && mc_placement_merge_slab(wet, bottom, 1, 0.5f, true) == full);
    for (int face = 0; face < 6; face++) {
        for (int h = 0; h <= 2; h++) {
            float y = 0.25f + h * 0.25f;
            bool b = face == 1 || (face >= 2 && y > 0.5f);
            bool t = face == 0 || (face >= 2 && y <= 0.5f);
            assert(mc_placement_merge_slab(bottom, bottom, face, y, true) == (b ? full : -1));
            assert(mc_placement_merge_slab(top, bottom, face, y, true) == (t ? full : -1));
            assert(mc_placement_merge_slab(bottom, top, face, y, false) == full);
            assert(mc_placement_resolve_simple(bottom, face, y) ==
                   (face == 0 || (face != 1 && y > 0.5f) ? top : bottom));
        }
    }
    assert(mc_placement_merge_slab(full, bottom, 1, 1, true) == -1);
    assert(mc_placement_merge_slab(log, bottom, 1, 1, true) == -1);
    double lo, hi;
    assert(mc_placement_slab_bounds(bottom, &lo, &hi) && lo == 0 && hi == 0.5);
    assert(mc_placement_slab_bounds(top, &lo, &hi) && lo == 0.5 && hi == 1);
    assert(mc_placement_slab_bounds(full, &lo, &hi) && lo == 0 && hi == 1);
    assert(!mc_placement_valid_hit(6, 0, 0, 0));
    assert(!mc_placement_valid_hit(1, NAN, 0, 0));
    assert(!mc_placement_valid_hit(1, 0, INFINITY, 0));
    assert(!mc_placement_valid_hit(1, 0, 2, 0));
    assert(mc_placement_valid_hit(1, 0, 0.5f, 1));
    assert(mc_placement_within_reach(-17, 65.62, 16, -16, 64, 15, 6));
    assert(!mc_placement_within_reach(0, 65, 0, 20, 64, 0, 6));

    mc_global_state_id_t air = expect_roundtrip("minecraft:air");
    mc_global_state_id_t stone = expect_roundtrip("minecraft:stone");
    mc_global_state_id_t chest_north = expect_roundtrip("minecraft:chest[facing=north,type=single,waterlogged=false]");
    mc_global_state_id_t chest_east = expect_roundtrip("minecraft:chest[facing=east,type=single,waterlogged=false]");

    assert((GLOBAL_BLOCK_STATES[air].flags & MC_BLOCK_FLAG_VALID) != 0u);
    assert((GLOBAL_BLOCK_STATES[air].flags & MC_BLOCK_FLAG_IS_DEFAULT_STATE) != 0u);
    assert((GLOBAL_BLOCK_STATES[air].flags & MC_BLOCK_FLAG_IS_AIR) != 0u);

    assert((GLOBAL_BLOCK_STATES[stone].flags & MC_BLOCK_FLAG_VALID) != 0u);
    assert((GLOBAL_BLOCK_STATES[stone].flags & MC_BLOCK_FLAG_IS_AIR) == 0u);
    assert((GLOBAL_BLOCK_STATES[stone].flags & MC_BLOCK_FLAG_IS_DEFAULT_STATE) != 0u);

    assert(GLOBAL_BLOCK_STATES[chest_north].block_index == GLOBAL_BLOCK_STATES[chest_east].block_index);
    assert(GLOBAL_BLOCKS[GLOBAL_BLOCK_STATES[chest_north].block_index].default_state == chest_north);

    assert(mc_global_state_id_from_key("minecraft:missing_block", 1234u) == 1234u);
    assert(mc_global_state_key(GLOBAL_BLOCK_STATES_COUNT + 100u) == NULL);

    puts("test_block_registry: ok");
    return 0;
}
