#include "mc_block_neighbors.h"
#include "mc_block_state.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *dir[] = {"north","east","south","west"};
static const int dx[] = {0,1,0,-1}, dz[] = {-1,0,1,0};
static int32_t cells[3][3];
static int unavailable;
static int read_cell(void *ctx, int32_t x, int32_t y, int32_t z, int32_t *out) {
    (void)ctx; (void)y;
    if (unavailable) return 1;
    assert(x >= -1 && x <= 1 && z >= -1 && z <= 1);
    *out = cells[x+1][z+1]; return 0;
}
static int32_t stair(const char *name, int facing, int top) {
    char key[256];
    snprintf(key,sizeof(key),"minecraft:%s[facing=%s,half=%s,shape=straight,waterlogged=false]",name,dir[facing],top?"top":"bottom");
    uint32_t state = mc_global_state_id_from_key(key,UINT32_MAX);
    assert(state != UINT32_MAX); return (int32_t)state;
}
static void shape_is(int32_t state, const char *expected) {
    int32_t result; char shape[32];
    assert(mc_block_neighbor_state(read_cell,NULL,0,64,0,state,&result)==0);
    assert(mc_block_state_property(result,"shape",shape,sizeof(shape)) && !strcmp(shape,expected));
    assert(mc_block_state_desc(result)==mc_block_state_desc(state));
    char a[16],b[16];
    assert(mc_block_state_property(state,"waterlogged",a,sizeof(a)));
    assert(mc_block_state_property(result,"waterlogged",b,sizeof(b)) && !strcmp(a,b));
}
int main(void) {
    for (int f=0; f<4; f++) for (int top=0; top<2; top++) {
        int32_t center=stair("oak_stairs",f,top);
        memset(cells,0,sizeof(cells));
        shape_is(center,"straight");
        for (int right=0; right<2; right++) {
            int n=(f+(right?1:3))&3;
            for (int back=0; back<2; back++) {
                memset(cells,0,sizeof(cells));
                int d=back?(f+2)&3:f;
                cells[dx[d]+1][dz[d]+1]=stair("stone_stairs",n,top);
                shape_is(center,back?(right?"inner_right":"inner_left"):(right?"outer_right":"outer_left"));
                int guard=back?n:(n+2)&3;
                cells[dx[guard]+1][dz[guard]+1]=stair("oak_stairs",f,top);
                shape_is(center,"straight");
                cells[dx[guard]+1][dz[guard]+1]=stair("oak_stairs",f,!top);
                shape_is(center,back?(right?"inner_right":"inner_left"):(right?"outer_right":"outer_left"));
                cells[dx[d]+1][dz[d]+1]=stair("stone_stairs",n,!top);
                shape_is(center,"straight");
            }
        }
        memset(cells,0,sizeof(cells));
        cells[dx[f]+1][dz[f]+1]=stair("oak_stairs",(f+3)&3,top);
        cells[dx[(f+2)&3]+1][dz[(f+2)&3]+1]=stair("oak_stairs",(f+1)&3,top);
        shape_is(mc_block_state_with_property(center,"waterlogged","true"),"outer_left");
        uint8_t mask=mc_stairs_collision_mask(center);
        int bits=0;for (int b=0;b<8;b++) bits+=(mask>>b)&1;
        assert(bits==6);
        const char *shapes[]={"outer_left","outer_right","inner_left","inner_right"};
        for (int s=0;s<4;s++) {
            mask=mc_stairs_collision_mask(mc_block_state_with_property(center,"shape",shapes[s]));
            bits=0;for (int b=0;b<8;b++) bits+=(mask>>b)&1;
            assert(bits==(s<2?5:7));
        }
    }
    for(int yaw=0;yaw<4;yaw++) for(int face=0;face<6;face++) for(int h=0;h<3;h++) {
        int32_t state=mc_stairs_placement(stair("oak_stairs",0,0),yaw*90.0f,face,0.25f+h*0.25f);
        char value[16];
        assert(mc_block_state_property(state,"facing",value,sizeof(value)) && !strcmp(value,dir[(yaw+2)&3]));
        assert(mc_block_state_property(state,"half",value,sizeof(value)) &&
               !strcmp(value,face==0 || (face!=1 && h==2)?"top":"bottom"));
    }
    assert(mc_stairs_placement(stair("oak_stairs",0,0),NAN,1,.5f)==-1);
    char wet[16];
    int32_t water = mc_block_placement_in_water(stair("oak_stairs",0,0),0);
    assert(mc_block_state_property(water,"waterlogged",wet,sizeof(wet)) && !strcmp(wet,"true"));
    water = mc_block_placement_in_water(stair("oak_stairs",0,0),8);
    assert(mc_block_state_property(water,"waterlogged",wet,sizeof(wet)) && !strcmp(wet,"false"));
    unavailable=1;int32_t out;
    assert(mc_block_neighbor_state(read_cell,NULL,0,64,0,stair("oak_stairs",0,0),&out)==1);
    assert(!mc_stairs_intersects(stair("oak_stairs",0,0),.1,.5,.6,.9,2,.9));
    assert(mc_stairs_intersects(stair("oak_stairs",0,0),.1,.5,.1,.9,2,.4));
    puts("stairs: placement, all rotations/halves/corners, blockers, precedence, voxels and unavailable reads ok");
}
