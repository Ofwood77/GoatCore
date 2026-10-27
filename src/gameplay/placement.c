#include "mc_placement.h"
#include "mc_block_state.h"
#include <math.h>
#include <string.h>

static bool slab_type(int32_t state, char type[16]) {
    const mc_block_desc_t *desc = mc_block_state_desc(state);
    if (!desc) return false;
    size_t n = strlen(desc->name);
    return n >= 5 && strcmp(desc->name + n - 5, "_slab") == 0 &&
           mc_block_state_property(state, "type", type, 16);
}

bool mc_placement_valid_hit(int32_t face, float x, float y, float z) {
    /* ServerGamePacketListenerImpl: distance from block center < 1.0000001. */
    return face >= 0 && face <= 5 && isfinite(x) && isfinite(y) && isfinite(z) &&
           fabs((double)x - 0.5) < 1.0000001 && fabs((double)y - 0.5) < 1.0000001 &&
           fabs((double)z - 0.5) < 1.0000001;
}

int32_t mc_placement_resolve_simple(int32_t state, int32_t face, float hit_y) {
    if (!mc_placement_valid_hit(face, 0.5f, hit_y, 0.5f)) return -1;
    char value[16];
    if (slab_type(state, value)) {
        const char *half = face == 0 || (face != 1 && hit_y > 0.5f) ? "top" : "bottom";
        return mc_block_state_with_property(state, "type", half);
    }
    if (mc_block_state_property(state, "axis", value, sizeof(value))) {
        return mc_block_state_with_property(state, "axis", face < 2 ? "y" : face < 4 ? "z" : "x");
    }
    return -1;
}

int32_t mc_placement_merge_slab(int32_t existing, int32_t requested, int32_t face,
                                float hit_y, bool replacing_clicked) {
    char type[16], other[16];
    if (!mc_placement_valid_hit(face, 0.5f, hit_y, 0.5f) ||
        !slab_type(existing, type) || !slab_type(requested, other) ||
        mc_block_state_desc(existing) != mc_block_state_desc(requested) || strcmp(type, "double") == 0) return -1;
    bool bottom = strcmp(type, "bottom") == 0;
    bool horizontal = face >= 2;
    if (replacing_clicked && !(bottom ? (face == 1 || (horizontal && hit_y > 0.5f)) :
                                       (face == 0 || (horizontal && hit_y <= 0.5f)))) return -1;
    int32_t result = mc_block_state_with_property(existing, "type", "double");
    return mc_block_state_with_property(result, "waterlogged", "false");
}

bool mc_placement_slab_bounds(int32_t state, double *min_y, double *max_y) {
    char type[16];
    if (!min_y || !max_y || !slab_type(state, type)) return false;
    *min_y = strcmp(type, "top") == 0 ? 0.5 : 0.0;
    *max_y = strcmp(type, "bottom") == 0 ? 0.5 : 1.0;
    return true;
}

bool mc_placement_within_reach(double ex, double ey, double ez,
                               int32_t x, int32_t y, int32_t z, double range) {
    double dx = fmax(fmax(x - ex, ex - ((double)x + 1)), 0);
    double dy = fmax(fmax(y - ey, ey - ((double)y + 1)), 0);
    double dz = fmax(fmax(z - ez, ez - ((double)z + 1)), 0);
    return isfinite(ex) && isfinite(ey) && isfinite(ez) && range > 0 &&
           dx * dx + dy * dy + dz * dz < range * range;
}
