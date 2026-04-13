#include "block_registry.h"

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
    assert(GLOBAL_BLOCK_COUNT > 1000u);
    assert(GLOBAL_BLOCK_STATES_COUNT > 20000u);

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
