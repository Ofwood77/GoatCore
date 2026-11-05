#include "mc_attached_world.h"
#include "mc_attached_blocks.h"
#include <math.h>
static int read_block(void *ctx,int32_t x,int32_t y,int32_t z,int32_t *state){
    return mc_world_get_block_ready(ctx,x,y,z,state);
}
int mc_attached_prepare(mc_world_t *w,int x,int y,int z,int32_t state,float yaw,float pitch,int face,bool replacing,int32_t *result){
    /* ServerGamePacketListenerImpl wraps packet rotations before storing them.
     * Keep this scoped to the new placement path; older movement rules stay put. */
    yaw=fmodf(yaw,360.0f);if(yaw>=180)yaw-=360;if(yaw< -180)yaw+=360;
    pitch=fmodf(pitch,360.0f);if(pitch>=180)pitch-=360;if(pitch< -180)pitch+=360;
    return mc_attached_placement(read_block,w,x,y,z,state,yaw,pitch,face,replacing,result);
}
