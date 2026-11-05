#ifndef MC_HINGED_WORLD_H
#define MC_HINGED_WORLD_H
#include "mc_world.h"
int mc_hinged_prepare(mc_world_t *w,int x,int y,int z,int32_t state,float yaw,int face,
                     double hit_x,float hit_y,double hit_z,bool replacing,int32_t *resolved);
int mc_hinged_apply(mc_world_t *w,int x,int y,int z,int32_t lower);
/* 0 pass, 1 handled, -1 refusal. Caller handles protocol and inventory. */
int mc_hinged_interact(mc_world_t *w,int x,int y,int z,int32_t state);
int mc_hinged_destroy(mc_world_t *w,int x,int y,int z,int32_t state,int32_t *loot_state);
#endif
