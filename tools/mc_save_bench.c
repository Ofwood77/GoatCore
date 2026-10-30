/* Remote backend probe: no listener/client, same codec/store as the tick. */
#ifdef NDEBUG
#error "mc_save_bench requires assertions: they execute and verify the fixed workload"
#endif
#include "mc_world.h"
#include "mc_anvil.h"
#include "mc_chunk_store.h"
#include "mc_block_state.h"
#include "mc_util.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <time.h>

static int32_t state(const char *name) {
    uint32_t id = mc_global_state_id_from_key(name, UINT32_MAX);
    assert(id != UINT32_MAX);
    return (int32_t)id;
}
static int ready(mc_world_t *w, int x, int y, int z) {
    for (int i = 0; i < 4000; i++) {
        int32_t value;
        mc_world_tick(w, 0);
        if (mc_world_get_block_ready(w, x, y, z, &value) == 0)
            return value;
        struct timespec t = {0, 1000000};
        nanosleep(&t, NULL);
    }
    return -1;
}
int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s MODE ROOT OPS COUNT\n", argv[0]);
        return 2;
    }
    const char *mode = argv[1], *path = argv[2];
    int ops = atoi(argv[3]), n = atoi(argv[4]);
    assert(ops > 0 && n > 0 && n <= 256);
    if (!strcmp(mode, "inspect") || !strcmp(mode, "recover")) {
        mc_block_entity_store_t store;
        mc_be_store_init(&store);
        mc_chunk_t chunk;
        int rc = mc_chunk_store_read(path, -32, -32, &chunk, &store);
        uint32_t value = 0, sentinel = 0;
        if (!rc) {
            value = mc_chunk_get_block(&chunk, 0, 64, 15);
            sentinel = mc_chunk_get_block(&chunk, 1, 64, 15);
            mc_chunk_destroy(&chunk);
        }
        if (!strcmp(mode, "recover")) {
            mc_world_t *w = mc_world_create_with_workers(path, 0, 1);
            assert(w);
            value = (uint32_t)ready(w, -512, 64, -497);
            sentinel = (uint32_t)ready(w, -511, 64, -497);
            mc_world_destroy(w);
        }
        printf("{\"read_rc\":%d,\"state\":%u,\"sentinel\":%u}\n", rc, value, sentinel);
        mc_be_store_destroy(&store);
        return 0;
    }
    int verify = !strncmp(mode, "verify", 6), update = !strncmp(mode, "update", 6);
    int region = !strncmp(mode, "region", 6);
    if (!verify && !update) {
        assert(mkdir(path, 0755) == 0);
    }
    mc_chunk_t *chunks = calloc((size_t)n, sizeof(*chunks));
    assert(chunks);
    mc_block_entity_store_t be;
    mc_be_store_init(&be);
    const char *names[] = {"minecraft:oak_log[axis=x]", "minecraft:oak_slab[type=top,waterlogged=false]",
                           "minecraft:oak_stairs[facing=east,half=top,shape=inner_right,waterlogged=true]",
                           "minecraft:stone"};
    int32_t states[4];
    for (int i = 0; i < 4; i++)
        states[i] = state(names[i]);
    char region_path[2048];
    snprintf(region_path, sizeof(region_path), "%s/r.-1.-1.mca", path);
    for (int i = 0; i < n; i++) {
        int cx, cz;
        if (strstr(mode, "multi")) {
            cx = (i - 8) * 32;
            cz = (i % 2) * 32;
        } else {
            cx = -32 + i % 16;
            cz = -32 + i / 16;
        }
        if (update || verify) {
            assert(mc_chunk_store_read(path, cx, cz, &chunks[i], &be) == 0);
        } else {
            assert(mc_chunk_init(&chunks[i], cx, cz, 0) == 0);
            /* Two mixed sections and 22 uniform air sections: a sparse floor fixture. */
            for (int z = 0; z < 16; z++)
                for (int x = 0; x < 16; x++)
                    assert(mc_chunk_set_block(&chunks[i], x, 63, z, (uint32_t)states[3]) == 0);
            assert(mc_chunk_set_block(&chunks[i], 0, 64, 15, (uint32_t)states[3]) == 0);
            assert(mc_chunk_set_block(&chunks[i], 1, 64, 15, (uint32_t)states[0]) == 0);
            assert(mc_chunk_store_write(path, &chunks[i], &be) == 0);
            if (region)
                assert(mc_anvil_write_chunk(region_path, &chunks[i], &be) == 0);
        }
    }
    mc_world_t *world = NULL;
    if (!strcmp(mode, "idle")) {
        world = mc_world_create_with_workers(path, 0, 1);
        assert(world);
        for (int i = 0; i < n; i++)
            assert(ready(world, chunks[i].cx * 16, 64, chunks[i].cz * 16) >= 0);
        for (int i = 0; i < 64; i++)
            mc_world_tick(world, 0);
    }
    int64_t start = mc_now_us();
    fprintf(stdout, "{\"phase\":\"start\",\"us\":%lld}\n", (long long)start);
    fflush(stdout);
    int writes = 0;
    if (!strcmp(mode, "idle")) {
        for (int i = 0; i < ops; i++) {
            mc_world_tick(world, 0);
            struct timespec t = {0, 50000000};
            nanosleep(&t, NULL);
        }
    } else if (verify) {
        for (int i = 0; i < ops; i++) {
            int idx = i % n;
            int32_t expected = states[(i / n) % 4];
            int last = i + (ops - 1 - i) / n * n;
            if (last != i)
                continue;
            assert(mc_chunk_get_block(&chunks[idx], 0, 64, 15) == (uint32_t)expected);
        }
    } else {
        for (int i = 0; i < ops; i++) {
            int idx = region ? 0 : i % n;
            assert(mc_chunk_set_block(&chunks[idx], 0, 64, 15, (uint32_t)states[(region ? i : i / n) % 4]) ==
                   0);
            if (region)
                assert(mc_anvil_write_chunk(region_path, &chunks[idx], &be) == 0);
            else
                assert(mc_chunk_store_write(path, &chunks[idx], &be) == 0);
            writes++;
        }
    }
    int64_t end = mc_now_us();
    fprintf(stdout, "{\"phase\":\"end\",\"us\":%lld,\"writes\":%d,\"duration_us\":%lld}\n", (long long)end,
            writes, (long long)(end - start));
    fflush(stdout);
    if (world)
        mc_world_destroy(world);
    for (int i = 0; i < n; i++) {
        if (region) {
            mc_chunk_t reloaded;
            mc_arena_t arena;
            assert(mc_arena_init(&arena, 2u * 1024u * 1024u) == 0);
            assert(mc_chunk_init(&reloaded, chunks[i].cx, chunks[i].cz, 0) == 0);
            assert(mc_anvil_load_chunk(region_path, chunks[i].cx, chunks[i].cz, &reloaded, &be, &arena) == 0);
            for (int y = MC_WORLD_MIN_Y; y < MC_WORLD_MIN_Y + MC_WORLD_HEIGHT; y++)
                for (int z = 0; z < 16; z++)
                    for (int x = 0; x < 16; x++)
                        assert(mc_chunk_get_block(&reloaded, x, y, z) ==
                               mc_chunk_get_block(&chunks[i], x, y, z));
            mc_chunk_destroy(&reloaded);
            mc_arena_destroy(&arena);
        }
        if (!verify && !region && strcmp(mode, "idle")) {
            mc_chunk_t readback;
            assert(mc_chunk_store_read(path, chunks[i].cx, chunks[i].cz, &readback, &be) == 0);
            for (int y = MC_WORLD_MIN_Y; y < MC_WORLD_MIN_Y + MC_WORLD_HEIGHT; y++)
                for (int z = 0; z < 16; z++)
                    for (int x = 0; x < 16; x++)
                        assert(mc_chunk_get_block(&readback, x, y, z) ==
                               mc_chunk_get_block(&chunks[i], x, y, z));
            mc_chunk_destroy(&readback);
        }
        mc_chunk_destroy(&chunks[i]);
    }
    mc_be_store_destroy(&be);
    free(chunks);
    return 0;
}
