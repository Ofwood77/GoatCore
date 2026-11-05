#ifndef MC_HINGED_BLOCKS_H
#define MC_HINGED_BLOCKS_H
#include "mc_block_neighbors.h"
#include <stddef.h>
typedef struct {
    const char *name;
    uint8_t kind, manual; /* 1 door, 2 trapdoor; official BlockSetType */
    int32_t item, open_sound, close_sound;
} mc_hinged_family_t;
const mc_hinged_family_t *mc_hinged_family(int32_t state);
bool mc_door_is_state(int32_t state);
bool mc_hinged_property(int32_t state,const char *name,const char *value);
/* Flags extracted against EmptyBlockGetter; never use as a general physics engine. */
enum {MC_SUPPORT_FULL=1,MC_SUPPORT_TOP=2,MC_SUPPORT_REPLACEABLE=4,MC_SUPPORT_SIGNAL=8};
unsigned mc_hinged_support_flags(int32_t state);
int32_t mc_hinged_placement(int32_t state,float yaw,int face,float hit_y,bool replacing);
/* Validate upper cell, sturdy floor and calculate the exact Vanilla hinge.
 * 0 success, 1 unavailable, -1 invalid. Caller validates lower replacement. */
int mc_door_placement(mc_block_neighbor_read_t read,void *ctx,int x,int y,int z,
                      int32_t state,double hit_x,double hit_z,int32_t *lower);
int mc_door_neighbor_change(mc_block_neighbor_read_t read,void *ctx,int x,int y,int z,
                            int32_t state,int changed_y,int32_t *result,bool *drop);
/* Bounds are local coordinates; all six faces are 3/16 thick. */
bool mc_hinged_bounds(int32_t state,double min[3],double max[3]);
bool mc_hinged_intersects(int32_t state,const double min[3],const double max[3]);
#endif
