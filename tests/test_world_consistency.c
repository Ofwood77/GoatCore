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

    mc_world_t *world = mc_world_create_with_workers(world_path, 1234, 2);
    assert(world);
    assert(mc_world_worker_count(world) == 2);
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

    assert(mc_world_set_block(world, 0, MC_WORLD_MIN_Y - 1, 0, ids->stone) == -1);
    assert(mc_world_set_block(world, 0, MC_WORLD_MIN_Y + MC_WORLD_HEIGHT, 0, ids->stone) == -1);
    /* Across both sides of negative chunk boundaries; save real variants. */
    int32_t variants[] = {
        mc_global_state_id_from_key("minecraft:oak_log[axis=x]", UINT32_MAX),
        mc_global_state_id_from_key("minecraft:oak_slab[type=top,waterlogged=false]", UINT32_MAX)
    };
    for (int i = 0; i < 2; i++) {
        int32_t nx = -17 + i, nz = 15 + i;
        assert(variants[i] >= 0);
        assert(wait_block_ready(world, nx, 64, nz, &state) == 0);
        assert(mc_world_set_block(world, nx, 64, nz, variants[i]) == 0);
        assert(mc_world_flush_block(world, nx, 64, nz) == 0);
    }
    mc_world_destroy(world);
    world = mc_world_create_with_workers(world_path, 1234, 2);
    assert(world);
    for (int i = 0; i < 2; i++) {
        assert(wait_block_ready(world, -17 + i, 64, 15 + i, &state) == 0);
        assert(state == variants[i]);
    }
    /* A front stair crosses the negative X chunk boundary. All writes, including
     * orientation edits and removal, must update the authoritative neighbors. */
    int32_t center = mc_global_state_id_from_key("minecraft:oak_stairs[facing=east,half=bottom,shape=straight,waterlogged=false]", UINT32_MAX);
    int32_t front = mc_global_state_id_from_key("minecraft:oak_stairs[facing=north,half=bottom,shape=straight,waterlogged=false]", UINT32_MAX);
    int32_t corner = mc_global_state_id_from_key("minecraft:oak_stairs[facing=east,half=bottom,shape=outer_left,waterlogged=false]", UINT32_MAX);
    for(int nx=-18;nx<=-15;nx++) for(int nz=13;nz<=15;nz++) {
        assert(wait_block_ready(world,nx,64,nz,&state)==0);
        assert(mc_world_set_block(world,nx,64,nz,0)==0);
    }
    assert(mc_world_set_block(world,-17,64,14,center)==0);
    mc_chunk_t *owner=mc_world_get_chunk(world,-2,0,0);
    assert(owner);
    uint64_t revision=owner->revision;
    assert(mc_world_set_block(world,-16,64,14,front)==0);
    assert(owner->revision>revision && owner->dirty);
    updates=mc_world_updates(world,&updates_len);
    bool announced=false;
    for(size_t i=0;i<updates_len;i++)
        if(updates[i].x==-17 && updates[i].y==64 && updates[i].z==14 && updates[i].state_id==corner) announced=true;
    assert(announced);
    assert(mc_world_get_block_ready(world,-17,64,14,&state)==0 && state==corner);
    assert(mc_world_flush_block(world,-17,64,14)==0);
    assert(mc_world_flush_block(world,-16,64,14)==0);
    mc_world_destroy(world);
    world=mc_world_create_with_workers(world_path,1234,2);
    assert(world);
    assert(wait_block_ready(world,-17,64,14,&state)==0 && state==corner);
    assert(wait_block_ready(world,-16,64,14,&state)==0);
    assert(mc_world_get_block_ready(world,-17,64,14,&state)==0 && state==corner);
    assert(mc_world_set_block(world,-16,64,14,0)==0);
    assert(mc_world_get_block_ready(world,-17,64,14,&state)==0 && state==center);
    mc_world_destroy(world);
    printf("test_world_consistency: ok\n");
    return 0;
}
