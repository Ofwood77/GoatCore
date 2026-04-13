#include "mc_mining.h"

#include "block_registry.h"

#include <assert.h>
#include <stdint.h>

static int32_t state_id(const char *key) {
    return (int32_t)mc_global_state_id_from_key(key, UINT32_MAX);
}

int main(void) {
    int32_t air = state_id("minecraft:air");
    int32_t dirt = state_id("minecraft:dirt");
    int32_t stone = state_id("minecraft:stone");
    int32_t bedrock = state_id("minecraft:bedrock");

    assert(air >= 0);
    assert(dirt >= 0);
    assert(stone >= 0);
    assert(bedrock >= 0);

    mc_mining_break_info_t air_info = mc_mining_break_info(air, NULL);
    assert(air_info.known_hardness);
    assert(!air_info.breakable);
    assert(air_info.instant);
    assert(air_info.required_ms == 0);

    mc_mining_break_info_t bedrock_info = mc_mining_break_info(bedrock, NULL);
    assert(bedrock_info.known_hardness);
    assert(!bedrock_info.breakable);
    assert(!mc_mining_elapsed_enough(&bedrock_info, 1000, 100000, NULL));

    mc_mining_break_info_t dirt_info = mc_mining_break_info(dirt, NULL);
    mc_mining_break_info_t stone_info = mc_mining_break_info(stone, NULL);
    assert(dirt_info.breakable);
    assert(stone_info.breakable);
    assert(dirt_info.required_ms > 0);
    assert(stone_info.required_ms > dirt_info.required_ms);

    int64_t elapsed_ms = 0;
    assert(!mc_mining_elapsed_enough(&stone_info, 1000, 1000 + stone_info.required_ms - 1, &elapsed_ms));
    assert(elapsed_ms == stone_info.required_ms - 1);
    assert(mc_mining_elapsed_enough(&stone_info, 1000, 1000 + stone_info.required_ms, &elapsed_ms));
    assert(elapsed_ms == stone_info.required_ms);

    mc_mining_break_info_t invalid_info = mc_mining_break_info(-1, NULL);
    assert(!invalid_info.breakable);

    return 0;
}
