#include "mc_chunk.h"
#include "mc_protocol.h"
#include "mc_world.h"
#include "mc_net.h"
#include "mc_server.h"
#include "mc_inventory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int conn_write_packet(mc_conn_t *c, int32_t packet_id, const uint8_t *payload, size_t payload_len, int compression_threshold) {
    (void)c;
    (void)packet_id;
    (void)payload;
    (void)payload_len;
    (void)compression_threshold;
    return 0;
}

void conn_close(mc_conn_t *c) {
    (void)c;
}

mc_conn_t *net_server_find_conn_by_name(mc_server_t *server, const char *name) {
    (void)server;
    (void)name;
    return NULL;
}

void net_server_release_conn(mc_conn_t *conn) {
    (void)conn;
}

mc_world_t *net_server_world(mc_server_t *server) {
    (void)server;
    return NULL;
}

int net_server_spawn_item_drop(mc_server_t *server, double x, double y, double z, const mc_slot_t *slot) {
    (void)server;
    (void)x;
    (void)y;
    (void)z;
    (void)slot;
    return 0;
}

int net_server_spawn_item_drop_locked(mc_server_t *server, double x, double y, double z, const mc_slot_t *slot) {
    return net_server_spawn_item_drop(server, x, y, z, slot);
}

int net_server_spawn_item_drop_with_motion(mc_server_t *server, double x, double y, double z, double vx, double vy, double vz,
                                           const mc_slot_t *slot, int32_t pickup_delay_ticks) {
    (void)vx;
    (void)vy;
    (void)vz;
    (void)pickup_delay_ticks;
    return net_server_spawn_item_drop(server, x, y, z, slot);
}

int net_server_sync_item_entities_to_conn(mc_server_t *server, mc_conn_t *conn) {
    (void)server;
    (void)conn;
    return 0;
}

int net_server_resolve_item_entities_for_block(mc_server_t *server, int32_t x, int32_t y, int32_t z, int32_t state_id) {
    (void)server;
    (void)x;
    (void)y;
    (void)z;
    (void)state_id;
    return 0;
}

mc_difficulty_t net_server_get_difficulty(mc_server_t *server) {
    (void)server;
    return MC_DIFFICULTY_NORMAL;
}

void net_server_set_difficulty(mc_server_t *server, mc_difficulty_t difficulty) {
    (void)server;
    (void)difficulty;
}

const char *mc_difficulty_name(mc_difficulty_t difficulty) {
    switch (difficulty) {
        case MC_DIFFICULTY_PEACEFUL: return "peaceful";
        case MC_DIFFICULTY_EASY: return "easy";
        case MC_DIFFICULTY_NORMAL: return "normal";
        case MC_DIFFICULTY_HARD: return "hard";
    }
    return "normal";
}

int net_server_broadcast_difficulty(mc_server_t *server) {
    (void)server;
    return 0;
}

void net_server_close_container_viewers(mc_server_t *server, mc_container_kind_t kind, int32_t x, int32_t y, int32_t z) {
    (void)server;
    (void)kind;
    (void)x;
    (void)y;
    (void)z;
}

int net_server_get_open_container_snapshot(mc_server_t *server, mc_container_kind_t kind, int32_t x, int32_t y, int32_t z,
                                           mc_container_instance_t *out) {
    (void)server;
    (void)kind;
    (void)x;
    (void)y;
    (void)z;
    (void)out;
    return 1;
}

static int fail(const char *msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
}

static int mk_temp_world(char *buf, size_t cap) {
    snprintf(buf, cap, "/tmp/mc_chunk_network_codec_XXXXXX");
    return mkdtemp(buf) ? 0 : -1;
}

static int run_case(mc_world_t *world, mc_chunk_t *chunk, const char *label) {
    mc_buf_t out;
    if (buf_init(&out, 8192) != 0) return fail("buf_init");
    if (proto_play_encode_chunkdata_for_test(world, chunk, &out) != 0) {
        buf_free(&out);
        fprintf(stderr, "FAIL: encode failed for %s\n", label);
        return 1;
    }
    if (proto_play_validate_chunkdata_for_test(out.data, out.len) != 0) {
        buf_free(&out);
        fprintf(stderr, "FAIL: validate failed for %s (len=%zu)\n", label, out.len);
        return 1;
    }
    buf_free(&out);
    return 0;
}

int main(void) {
    char world_path[128];
    if (mk_temp_world(world_path, sizeof(world_path)) != 0) return fail("mkdtemp");

    mc_world_t *world = mc_world_create(world_path, 1234);
    if (!world) return fail("mc_world_create");

    const mc_world_ids_t *ids = mc_world_ids(world);
    if (!ids) return fail("mc_world_ids");

    mc_chunk_t chunk;
    if (mc_chunk_init(&chunk, 0, 0, (mc_global_state_id_t)ids->air) != 0) return fail("mc_chunk_init");

    if (run_case(world, &chunk, "uniform air") != 0) {
        mc_chunk_destroy(&chunk);
        mc_world_destroy(world);
        return 1;
    }

    if (mc_chunk_set_block(&chunk, 3, 12, 8, (mc_global_state_id_t)ids->stone) != 0) {
        mc_chunk_destroy(&chunk);
        mc_world_destroy(world);
        return fail("set local palette block");
    }
    if (run_case(world, &chunk, "single local palette section") != 0) {
        mc_chunk_destroy(&chunk);
        mc_world_destroy(world);
        return 1;
    }

    for (int i = 0; i < 300; i++) {
        int x = i & 15;
        int z = (i >> 4) & 15;
        int y = 32 + ((i >> 8) & 15);
        if (mc_chunk_set_block(&chunk, x, y, z, (mc_global_state_id_t)(i + 1)) != 0) {
            mc_chunk_destroy(&chunk);
            mc_world_destroy(world);
            return fail("set global palette fallback block");
        }
    }
    if (run_case(world, &chunk, "global palette section") != 0) {
        mc_chunk_destroy(&chunk);
        mc_world_destroy(world);
        return 1;
    }

    mc_chunk_destroy(&chunk);
    mc_world_destroy(world);
    printf("test_chunk_network_codec: ok\n");
    return 0;
}
