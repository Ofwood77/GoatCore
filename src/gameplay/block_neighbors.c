#include "mc_block_neighbors.h"
#include "mc_block_state.h"
#include "mc_hinged_blocks.h"
#include "mc_attached_blocks.h"
#include <math.h>
#include <string.h>

/* Clockwise N,E,S,W; registry lookup supplies every state ID. */
static const char *directions[] = {"north", "east", "south", "west"};
static const int dx[] = {0, 1, 0, -1}, dz[] = {-1, 0, 1, 0};
bool mc_stairs_is_state(int32_t state) {
    const mc_block_desc_t *b = mc_block_state_desc(state);
    if (!b) return false;
    size_t n = strlen(b->name);
    return n >= 7 && !strcmp(b->name + n - 7, "_stairs");
}
static int facing(int32_t state) {
    char value[16];
    if (!mc_stairs_is_state(state) || !mc_block_state_property(state, "facing", value, sizeof(value))) return -1;
    for (int i = 0; i < 4; i++) if (!strcmp(value, directions[i])) return i;
    return -1;
}
static bool same_half(int32_t a, int32_t b) {
    char x[16], y[16];
    return mc_stairs_is_state(b) && mc_block_state_property(a, "half", x, sizeof(x)) &&
        mc_block_state_property(b, "half", y, sizeof(y)) && !strcmp(x, y);
}
int32_t mc_block_placement_in_water(int32_t state, int level) {
    /* StairBlock tests Fluids.WATER, excluding FLOWING_WATER. Other families
     * retain their existing placement policy for this stair-only change. */
    return mc_block_state_with_property(state, "waterlogged", (mc_stairs_is_state(state) || mc_attached_family(state) || (mc_hinged_family(state) && mc_hinged_family(state)->kind==2)) && level != 0 ? "false" : "true");
}
int32_t mc_stairs_placement(int32_t state, float yaw, int32_t face, float hit_y) {
    if (!mc_stairs_is_state(state) || !isfinite(yaw) || !isfinite(hit_y) || face < 0 || face > 5) return -1;
    int dir = ((int)floorf(remainderf(yaw, 360.0f) / 90.0f + 0.5f) + 2) & 3;
    state = mc_block_state_with_property(state, "facing", directions[dir]);
    state = mc_block_state_with_property(state, "half", face == 0 || (face != 1 && hit_y > 0.5f) ? "top" : "bottom");
    return mc_block_state_with_property(state, "shape", "straight");
}
int mc_block_neighbor_state(mc_block_neighbor_read_t read, void *ctx,
                            int32_t x, int32_t y, int32_t z, int32_t state, int32_t *result) {
    if (!result || !read) return -1;
    *result = state;
    int f = facing(state);
    if (f < 0) return 0;
    /* StairBlock.getStairsShape / canTakeShape, official 26.1.1 JAR.
     * Front outer corner wins over the back inner corner. A same-facing,
     * same-half stair at the side vetoes the corresponding corner. */
    const char *shape = "straight";
    for (int back = 0; back < 2; back++) {
        int d = back ? (f + 2) & 3 : f;
        int32_t neighbor;
        int rc = read(ctx, x + dx[d], y, z + dz[d], &neighbor);
        if (rc) return rc;
        int nf = facing(neighbor);
        if (nf < 0 || !same_half(state, neighbor) || (nf & 1) == (f & 1)) continue;
        int guard = back ? nf : (nf + 2) & 3;
        int32_t side;
        rc = read(ctx, x + dx[guard], y, z + dz[guard], &side);
        if (rc) return rc;
        if (facing(side) == f && same_half(state, side)) continue;
        bool left = nf == ((f + 3) & 3);
        shape = back ? (left ? "inner_left" : "inner_right") : (left ? "outer_left" : "outer_right");
        break;
    }
    *result = mc_block_state_with_property(state, "shape", shape);
    return *result < 0 ? -1 : 0;
}
uint8_t mc_stairs_collision_mask(int32_t state) {
    int f = facing(state);
    char half[16], shape[32];
    if (f < 0 || !mc_block_state_property(state, "half", half, sizeof(half)) ||
        !mc_block_state_property(state, "shape", shape, sizeof(shape))) return 0;
    const uint8_t step[] = {3, 10, 12, 5};
    uint8_t mask = step[f];
    if (!strncmp(shape, "outer_", 6)) mask &= step[(f + (!strcmp(shape, "outer_left") ? 3 : 1)) & 3];
    if (!strncmp(shape, "inner_", 6)) mask |= step[(f + (!strcmp(shape, "inner_left") ? 3 : 1)) & 3];
    return !strcmp(half, "bottom") ? (uint8_t)(15 | mask << 4) : (uint8_t)(240 | mask);
}
bool mc_stairs_intersects(int32_t state, double x0, double y0, double z0, double x1, double y1, double z1) {
    uint8_t mask = mc_stairs_collision_mask(state);
    for (int i = 0; i < 8; i++) {
        if (!(mask & (1u << i))) continue;
        double x = (i & 1) * 0.5, z = ((i >> 1) & 1) * 0.5, y = (i >> 2) * 0.5;
        if (x0 < x + 0.5 - 1e-6 && x1 > x + 1e-6 && y0 < y + 0.5 - 1e-6 && y1 > y + 1e-6 &&
            z0 < z + 0.5 - 1e-6 && z1 > z + 1e-6) return true;
    }
    return false;
}
