#include "mc_hinged_world.h"
#include "mc_hinged_blocks.h"
#include "mc_block_state.h"
static int read_block(void *ctx,int32_t x,int32_t y,int32_t z,int32_t *state){
    if(y< -64||y>=320){*state=mc_world_ids(ctx)->air;return 0;}
    return mc_world_get_block_ready(ctx,x,y,z,state);
}
int mc_hinged_prepare(mc_world_t *w,int x,int y,int z,int32_t state,float yaw,int face,
                     double hit_x,float hit_y,double hit_z,bool replacing,int32_t *resolved){
    int32_t next=mc_hinged_placement(state,yaw,face,hit_y,replacing);if(next<0)return -1;
    /* No redstone simulator exists: refuse unsupported adjacent signal sources
     * instead of claiming that an unpowered initial state was resolved exactly. */
    static const int offsets[6][3]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for(int h=0;h<(mc_door_is_state(next)?2:1);h++)for(int i=0;i<6;i++){
        int32_t other;int rc=read_block(w,x+offsets[i][0],y+h+offsets[i][1],z+offsets[i][2],&other);
        if(rc)return -1;
        if(mc_hinged_support_flags(other)&MC_SUPPORT_SIGNAL)return -1;
    }
    if(mc_door_is_state(next)){
        int rc=mc_door_placement(read_block,w,x,y,z,next,hit_x,hit_z,&next);if(rc)return -1;
    }
    *resolved=next;return 0;
}
int mc_hinged_apply(mc_world_t *w,int x,int y,int z,int32_t lower){
    if(!mc_door_is_state(lower))return mc_world_set_block(w,x,y,z,lower);
    return mc_world_set_block_pair(w,x,y,z,lower,mc_block_state_with_property(lower,"half","upper"));
}
static int pair(mc_world_t *w,int x,int y,int z,int32_t state,int *bottom,int32_t *lower){
    bool low=mc_hinged_property(state,"half","lower");*bottom=y-(low?0:1);
    int32_t other;
    if(read_block(w,x,y+(low?1:-1),z,&other)||!mc_door_is_state(other)||
       mc_block_state_desc(state)!=mc_block_state_desc(other)||
       !mc_hinged_property(other,"half",low?"upper":"lower"))return -1;
    *lower=low?state:other;return 0;
}
int mc_hinged_interact(mc_world_t *w,int x,int y,int z,int32_t state){
    const mc_hinged_family_t *f=mc_hinged_family(state);if(!f||!f->manual)return 0;
    int32_t next=mc_block_state_with_property(state,"open",mc_hinged_property(state,"open","true")?"false":"true");
    int rc;
    if(f->kind==1){int bottom;int32_t lower;if(pair(w,x,y,z,state,&bottom,&lower))return -1;
        next=mc_block_state_with_property(next,"half","lower");
        rc=mc_hinged_apply(w,x,bottom,z,next);
    }else rc=mc_world_set_block(w,x,y,z,next);
    if(rc||mc_world_request_block_save(w,x,y,z))return -1;
    return 1;
}
int mc_hinged_destroy(mc_world_t *w,int x,int y,int z,int32_t state,int32_t *loot_state){
    if(!mc_door_is_state(state))return 0;
    int bottom;int32_t lower;
    if(pair(w,x,y,z,state,&bottom,&lower))return -1;
    int32_t air=mc_world_ids(w)->air;
    if(mc_world_set_block_pair(w,x,bottom,z,air,air))return -1;
    *loot_state=lower;return 1;
}
