#ifndef MC_BLOCK_NEIGHBORS_H
#define MC_BLOCK_NEIGHBORS_H
#include <stdbool.h>
#include <stdint.h>

/* Tick-owned, read-only view: 0 = value, 1 = unavailable, -1 = error. */
typedef int (*mc_block_neighbor_read_t)(void *ctx, int32_t x, int32_t y, int32_t z, int32_t *state);
bool mc_stairs_is_state(int32_t state);
int32_t mc_block_placement_in_water(int32_t state, int level);
int32_t mc_stairs_placement(int32_t state, float yaw, int32_t face, float hit_y);
/* Pure local rule dispatch. No live mutation, I/O, network or scheduled ticks. */
int mc_block_neighbor_state(mc_block_neighbor_read_t read, void *ctx,
                            int32_t x, int32_t y, int32_t z, int32_t state, int32_t *result);
/* Eight half-block voxels, bit x + 2*z + 4*y, only for stairs. */
uint8_t mc_stairs_collision_mask(int32_t state);
bool mc_stairs_intersects(int32_t state, double x0, double y0, double z0,
                          double x1, double y1, double z1);
#endif
