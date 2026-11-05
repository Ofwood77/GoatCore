#ifndef MC_ATTACHED_BLOCKS_H
#define MC_ATTACHED_BLOCKS_H
#include "mc_block_neighbors.h"
#include <stddef.h>
/* Direction IDs are Vanilla DOWN, UP, NORTH, SOUTH, WEST, EAST. */
typedef enum {MC_FACE_FULL, MC_FACE_CENTER, MC_FACE_RIGID} mc_face_support_t;
bool mc_support_face(int32_t state, int face, mc_face_support_t type);
/* Includes the unstable_bottom_center exclusion for CENTER/DOWN. */
typedef struct {
    const char *name;
    int kind; /* 1 standing torch, 2 wall torch, 3 lantern, 4 ladder */
    int32_t item, default_state;
} mc_attached_family_t;
const mc_attached_family_t *mc_attached_family(int32_t state);
/* Exact quantized official look ordering. Returns -1 for non-finite input. */
int mc_placement_directions(float yaw,float pitch,int face,bool replacing,int order[6]);
/* -1 invalid/read error, 1 unavailable; neither is interpreted as "air".
 * On survival success, result means valid/invalid; rc alone is availability. */
int mc_attached_survives(mc_block_neighbor_read_t read,void *ctx,int x,int y,int z,
                         int32_t state,bool *result);
int mc_attached_support_direction(int32_t state);
/* Caller validates destination replacement and collision, and applies water. */
int mc_attached_placement(mc_block_neighbor_read_t read,void *ctx,int x,int y,int z,
                          int32_t state,float yaw,float pitch,int face,bool replacing,int32_t *result);
typedef struct { double min[3],max[3]; } mc_block_box_t;
/* Selection and collision are deliberately distinct (official 1/32 units). Returns 0..2 or -1. */
int mc_attached_boxes(int32_t state,bool collision,mc_block_box_t boxes[2]);
bool mc_attached_intersects(int32_t state,const double min[3],const double max[3]);
int mc_attached_light(int32_t state);
#endif
