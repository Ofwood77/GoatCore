#include "mc_nbt.h"

#include <stdio.h>
#include <string.h>

static int fail(const char *msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
}

static void write_u16_be(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xFF);
    p[1] = (uint8_t)(v & 0xFF);
}

static void write_i32_be(uint8_t *p, int32_t v) {
    uint32_t u = (uint32_t)v;
    p[0] = (uint8_t)((u >> 24) & 0xFF);
    p[1] = (uint8_t)((u >> 16) & 0xFF);
    p[2] = (uint8_t)((u >> 8) & 0xFF);
    p[3] = (uint8_t)(u & 0xFF);
}

static int test_arena_basic(void) {
    mc_arena_t arena;
    void *a = NULL;
    void *b = NULL;

    if (mc_arena_init(&arena, 1024) != 0) return fail("arena init");
    a = mc_arena_alloc(&arena, 24);
    b = mc_arena_alloc(&arena, 32);
    if (!a || !b) {
        mc_arena_destroy(&arena);
        return fail("arena alloc");
    }
    if (a == b) {
        mc_arena_destroy(&arena);
        return fail("arena distinct allocations");
    }
    if (arena.offset == 0) {
        mc_arena_destroy(&arena);
        return fail("arena offset not advanced");
    }

    mc_arena_reset(&arena);
    if (arena.offset != 0) {
        mc_arena_destroy(&arena);
        return fail("arena reset offset");
    }

    a = mc_arena_alloc(&arena, 16);
    if (!a || a != arena.base) {
        mc_arena_destroy(&arena);
        return fail("arena reset reuse");
    }

    mc_arena_destroy(&arena);
    return 0;
}

static int test_nbt_parse_and_validate(void) {
    uint8_t buf[64];
    size_t pos = 0;
    size_t consumed = 0;
    mc_arena_t arena;
    mc_nbt_tag_t *root = NULL;
    const mc_nbt_tag_t *version = NULL;

    buf[pos++] = 0x0A;
    write_u16_be(buf + pos, 0);
    pos += 2;

    buf[pos++] = 0x03;
    write_u16_be(buf + pos, 11);
    pos += 2;
    memcpy(buf + pos, "DataVersion", 11);
    pos += 11;
    write_i32_be(buf + pos, 4785);
    pos += 4;

    buf[pos++] = 0x00;

    if (mc_arena_init(&arena, 4096) != 0) return fail("arena init parse");
    if (mc_nbt_read_named_root_arena(buf, pos, &arena, &root, &consumed) != 0) {
        mc_arena_destroy(&arena);
        return fail("arena parse");
    }
    if (consumed != pos) {
        mc_arena_destroy(&arena);
        return fail("arena parse consumed");
    }
    if (!root || root->type != MC_NBT_TAG_COMPOUND) {
        mc_arena_destroy(&arena);
        return fail("arena root compound");
    }

    version = mc_nbt_compound_get(root, "DataVersion");
    if (!version || version->type != MC_NBT_TAG_INT || version->payload.int_val != 4785) {
        mc_arena_destroy(&arena);
        return fail("arena DataVersion read");
    }
    if (!mc_anvil_validate_chunk(root)) {
        mc_arena_destroy(&arena);
        return fail("arena validate chunk");
    }

    mc_arena_reset(&arena);
    if (arena.offset != 0) {
        mc_arena_destroy(&arena);
        return fail("arena reset after parse");
    }

    mc_arena_destroy(&arena);
    return 0;
}

static int test_nbt_validate_rejects_wrong_version(void) {
    uint8_t buf[64];
    size_t pos = 0;
    mc_arena_t arena;
    mc_nbt_tag_t *root = NULL;

    buf[pos++] = 0x0A;
    write_u16_be(buf + pos, 0);
    pos += 2;

    buf[pos++] = 0x03;
    write_u16_be(buf + pos, 11);
    pos += 2;
    memcpy(buf + pos, "DataVersion", 11);
    pos += 11;
    write_i32_be(buf + pos, 1);
    pos += 4;
    buf[pos++] = 0x00;

    if (mc_arena_init(&arena, 4096) != 0) return fail("arena init reject");
    if (mc_nbt_read_named_root_arena(buf, pos, &arena, &root, NULL) != 0) {
        mc_arena_destroy(&arena);
        return fail("arena parse reject");
    }
    if (mc_anvil_validate_chunk(root)) {
        mc_arena_destroy(&arena);
        return fail("validate accepted wrong version");
    }

    mc_arena_destroy(&arena);
    return 0;
}

int main(void) {
    if (test_arena_basic() != 0) return 1;
    if (test_nbt_parse_and_validate() != 0) return 1;
    if (test_nbt_validate_rejects_wrong_version() != 0) return 1;
    return 0;
}
