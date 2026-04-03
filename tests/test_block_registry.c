#include "block_registry.h"

#include <assert.h>
#include <stdio.h>

static void expect_roundtrip(const char *key, mc_global_state_id_t expected_id) {
    mc_global_state_id_t actual_id = mc_global_state_id_from_key(key, 9999u);
    assert(actual_id == expected_id);
    assert(mc_global_state_key(expected_id) != NULL);
}

int main(void) {
    assert(GLOBAL_BLOCK_COUNT == 4u);
    assert(GLOBAL_BLOCK_STATES_COUNT == 7u);

    assert((GLOBAL_BLOCK_STATES[0].flags & MC_BLOCK_FLAG_VALID) != 0u);
    assert((GLOBAL_BLOCK_STATES[0].flags & MC_BLOCK_FLAG_IS_DEFAULT_STATE) != 0u);
    assert((GLOBAL_BLOCK_STATES[0].flags & MC_BLOCK_FLAG_IS_AIR) != 0u);

    assert((GLOBAL_BLOCK_STATES[1].flags & MC_BLOCK_FLAG_VALID) != 0u);
    assert((GLOBAL_BLOCK_STATES[1].flags & MC_BLOCK_FLAG_IS_AIR) == 0u);

    assert((GLOBAL_BLOCK_STATES[3].flags & MC_BLOCK_FLAG_IS_DEFAULT_STATE) != 0u);
    assert((GLOBAL_BLOCK_STATES[4].flags & MC_BLOCK_FLAG_IS_DEFAULT_STATE) == 0u);
    assert(GLOBAL_BLOCK_STATES[3].block_index == GLOBAL_BLOCK_STATES[4].block_index);
    assert(GLOBAL_BLOCKS[GLOBAL_BLOCK_STATES[3].block_index].default_state == 3u);

    expect_roundtrip("minecraft:air", 0u);
    expect_roundtrip("minecraft:stone", 1u);
    expect_roundtrip("minecraft:glass", 2u);
    expect_roundtrip("minecraft:chest[facing=east]", 6u);
    expect_roundtrip("minecraft:chest[facing=north]", 3u);

    assert(mc_global_state_id_from_key("minecraft:missing_block", 1234u) == 1234u);
    assert(mc_global_state_key(99u) == NULL);

    puts("test_block_registry: ok");
    return 0;
}
