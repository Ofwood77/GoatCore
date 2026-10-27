#ifndef MC_PLACEMENT_H
#define MC_PLACEMENT_H
#include <stdbool.h>
#include <stdint.h>

bool mc_placement_valid_hit(int32_t face, float x, float y, float z);
/* Pure state resolution for axis blocks and slabs; -1 for other families. */
int32_t mc_placement_resolve_simple(int32_t default_state, int32_t face, float hit_y);
/* replacing_clicked=false implements replacement in the adjacent cell. */
int32_t mc_placement_merge_slab(int32_t existing, int32_t requested, int32_t face,
                                float hit_y, bool replacing_clicked);
bool mc_placement_slab_bounds(int32_t state, double *min_y, double *max_y);
bool mc_placement_within_reach(double eye_x, double eye_y, double eye_z,
                               int32_t x, int32_t y, int32_t z, double range);
#endif
