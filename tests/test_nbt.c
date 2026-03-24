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

static int test_compound_coords(void) {
    uint8_t buf[64];
    size_t pos = 0;

    buf[pos++] = 0x0A; /* root compound */
    write_u16_be(buf + pos, 0); /* empty name */
    pos += 2;

    buf[pos++] = 0x03; /* TAG_Int xPos */
    write_u16_be(buf + pos, 4);
    pos += 2;
    memcpy(buf + pos, "xPos", 4);
    pos += 4;
    write_i32_be(buf + pos, 42);
    pos += 4;

    buf[pos++] = 0x03; /* TAG_Int zPos */
    write_u16_be(buf + pos, 4);
    pos += 2;
    memcpy(buf + pos, "zPos", 4);
    pos += 4;
    write_i32_be(buf + pos, -7);
    pos += 4;

    buf[pos++] = 0x00; /* TAG_End */

    mc_nbt_tag_t *root = NULL;
    size_t consumed = 0;
    if (mc_nbt_read_named_root(buf, pos, &root, &consumed) != 0) return fail("parse coords compound");
    if (consumed != pos) return fail("consumed mismatch");
    if (!root || root->type != MC_NBT_TAG_COMPOUND) return fail("root is not compound");

    const mc_nbt_tag_t *x = mc_nbt_compound_get(root, "xPos");
    const mc_nbt_tag_t *z = mc_nbt_compound_get(root, "zPos");
    if (!x || x->type != MC_NBT_TAG_INT || x->payload.int_val != 42) return fail("xPos mismatch");
    if (!z || z->type != MC_NBT_TAG_INT || z->payload.int_val != -7) return fail("zPos mismatch");

    mc_nbt_free(root);
    return 0;
}

static int test_list_ints(void) {
    uint8_t buf[128];
    size_t pos = 0;

    buf[pos++] = 0x0A; /* root compound */
    write_u16_be(buf + pos, 0);
    pos += 2;

    buf[pos++] = 0x09; /* TAG_List ints */
    write_u16_be(buf + pos, 4);
    pos += 2;
    memcpy(buf + pos, "ints", 4);
    pos += 4;
    buf[pos++] = 0x03; /* element type = int */
    write_i32_be(buf + pos, 3);
    pos += 4;
    write_i32_be(buf + pos, 1);
    pos += 4;
    write_i32_be(buf + pos, 2);
    pos += 4;
    write_i32_be(buf + pos, 3);
    pos += 4;

    buf[pos++] = 0x00; /* TAG_End */

    mc_nbt_tag_t *root = NULL;
    size_t consumed = 0;
    if (mc_nbt_read_named_root(buf, pos, &root, &consumed) != 0) return fail("parse list compound");
    if (!root || root->type != MC_NBT_TAG_COMPOUND) return fail("root is not compound");

    const mc_nbt_tag_t *ints = mc_nbt_compound_get(root, "ints");
    if (!ints || ints->type != MC_NBT_TAG_LIST) return fail("ints list missing");
    if (ints->payload.list.elem_type != MC_NBT_TAG_INT) return fail("ints elem type mismatch");
    if (ints->payload.list.length != 3) return fail("ints length mismatch");
    if (!ints->payload.list.items) return fail("ints items missing");
    if (ints->payload.list.items[0]->payload.int_val != 1) return fail("ints[0] mismatch");
    if (ints->payload.list.items[1]->payload.int_val != 2) return fail("ints[1] mismatch");
    if (ints->payload.list.items[2]->payload.int_val != 3) return fail("ints[2] mismatch");

    mc_nbt_free(root);
    return 0;
}

int main(void) {
    if (test_compound_coords() != 0) return 1;
    if (test_list_ints() != 0) return 1;
    return 0;
}
