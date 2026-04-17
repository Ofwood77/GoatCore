#include "mc_world.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static int mk_temp_world(char *buf, size_t cap) {
    snprintf(buf, cap, "/tmp/mc_world_consistency_XXXXXX");
    return mkdtemp(buf) ? 0 : -1;
}

static int wait_block_ready(mc_world_t *world, int32_t x, int32_t y, int32_t z, int32_t *out_state_id) {
    for (int i = 0; i < 400; i++) {
        mc_world_tick(world, 0);
        int rc = mc_world_get_block_ready(world, x, y, z, out_state_id);
        if (rc == 0) return 0;
        if (rc < 0) return -1;
        struct timespec ts = {0};
        ts.tv_nsec = 1000000;
        nanosleep(&ts, NULL);
    }
    return -1;
}

int main(void) {
    char world_path[128];
    assert(mk_temp_world(world_path, sizeof(world_path)) == 0);

    mc_world_t *world = mc_world_create(world_path, 1234);
    assert(world);
    const mc_world_ids_t *ids = mc_world_ids(world);
    assert(ids);

    int32_t x = 1024;
    int32_t y = 250;
    int32_t z = 1024;
    int32_t state = -12345;
    assert(mc_world_get_block_ready(world, x, y, z, &state) == 1);
    assert(state == -12345);
    assert(wait_block_ready(world, x, y, z, &state) == 0);

    mc_world_clear_updates(world);

    int32_t first_state = state == ids->stone ? ids->dirt : ids->stone;
    int32_t second_state = first_state == ids->redstone_block ? ids->dirt : ids->redstone_block;
    assert(first_state != second_state);

    assert(mc_world_set_block(world, x, y, z, first_state) == 0);
    assert(mc_world_set_block(world, x, y, z, second_state) == 0);

    size_t updates_len = 0;
    const mc_block_update_t *updates = mc_world_updates(world, &updates_len);
    assert(updates);
    assert(updates_len == 1);
    assert(updates[0].x == x);
    assert(updates[0].y == y);
    assert(updates[0].z == z);
    assert(updates[0].state_id == second_state);

    assert(mc_world_set_block(world, x + 1, y, z, first_state) == 0);
    updates = mc_world_updates(world, &updates_len);
    assert(updates);
    assert(updates_len == 2);

    mc_world_clear_updates(world);
    updates = mc_world_updates(world, &updates_len);
    assert(updates_len == 0);

    state = -12345;
    assert(mc_world_get_block_ready(world, x, MC_WORLD_MIN_Y - 1, z, &state) == 0);
    assert(state == ids->air);

    mc_world_destroy(world);
    printf("test_world_consistency: ok\n");
    return 0;
}
