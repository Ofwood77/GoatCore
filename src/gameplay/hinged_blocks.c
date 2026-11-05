#include "mc_hinged_blocks.h"
#include "mc_block_state.h"
#include <math.h>
#include <string.h>
#include "generated_hinged_data.h"
static const char *dirs[]={"north","east","south","west"};
static const int dx[]={0,1,0,-1},dz[]={-1,0,1,0};
const mc_hinged_family_t *mc_hinged_family(int32_t state){
    const mc_block_desc_t *b=mc_block_state_desc(state);if(!b)return NULL;
    size_t lo=0,hi=sizeof(families)/sizeof(*families);
    while(lo<hi){size_t mid=lo+(hi-lo)/2;int cmp=strcmp(b->name,families[mid].name);
        if(!cmp)return &families[mid];
        if(cmp<0)hi=mid;else lo=mid+1;}
    return NULL;
}
bool mc_door_is_state(int32_t state){const mc_hinged_family_t *f=mc_hinged_family(state);return f&&f->kind==1;}
bool mc_hinged_property(int32_t state,const char *name,const char *value){
    char text[32];return mc_block_state_property(state,name,text,sizeof(text))&&!strcmp(text,value);
}
unsigned mc_hinged_support_flags(int32_t state){return state>=0&&state<29873?support_flags[state]:0;}
static int direction(int32_t state){for(int i=0;i<4;i++)if(mc_hinged_property(state,"facing",dirs[i]))return i;return -1;}
int32_t mc_hinged_placement(int32_t state,float yaw,int face,float hit_y,bool replacing){
    const mc_hinged_family_t *f=mc_hinged_family(state);
    if(!f||!isfinite(yaw)||!isfinite(hit_y)||face<0||face>5)return -1;
    int d=((int)floorf(remainderf(yaw,360)/90+0.5f)+2)&3;
    if(f->kind==2){
        if(!replacing&&face>=2){static const int face_dir[]={0,0,0,2,3,1};d=face_dir[face];}
        else d=(d+2)&3;
        const char *half=(!replacing&&face>=2)?(hit_y>0.5f?"top":"bottom"):(face==1?"bottom":"top");
        state=mc_block_state_with_property(state,"half",half);
    }else state=mc_block_state_with_property(state,"half","lower");
    state=mc_block_state_with_property(state,"facing",dirs[d]);
    state=mc_block_state_with_property(state,"open","false");
    return mc_block_state_with_property(state,"powered","false");
}
int mc_door_placement(mc_block_neighbor_read_t read,void *ctx,int x,int y,int z,
                      int32_t state,double hit_x,double hit_z,int32_t *lower){
    if(!read||!lower||!mc_door_is_state(state)||y>=319||y< -64)return -1;
    int32_t top,floor,s[4];int rc=read(ctx,x,y+1,z,&top);if(rc)return rc;
    if(!(mc_hinged_support_flags(top)&MC_SUPPORT_REPLACEABLE))return -1;
    rc=read(ctx,x,y-1,z,&floor);if(rc)return rc;
    if(!(mc_hinged_support_flags(floor)&MC_SUPPORT_TOP))return -1;
    int f=direction(state);if(f<0)return -1;
    int left=(f+3)&3,right=(f+1)&3;
    for(int i=0;i<4;i++){
        int d=i<2?left:right;rc=read(ctx,x+dx[d],y+(i&1),z+dz[d],&s[i]);if(rc)return rc;
    }
    int score= -!!(mc_hinged_support_flags(s[0])&MC_SUPPORT_FULL)-!!(mc_hinged_support_flags(s[1])&MC_SUPPORT_FULL)
               +!!(mc_hinged_support_flags(s[2])&MC_SUPPORT_FULL)+!!(mc_hinged_support_flags(s[3])&MC_SUPPORT_FULL);
    bool ld=mc_door_is_state(s[0])&&mc_hinged_property(s[0],"half","lower");
    bool rd=mc_door_is_state(s[2])&&mc_hinged_property(s[2],"half","lower");
    bool hinge;
    if((ld&&!rd)||score>0)hinge=true;
    else if((rd&&!ld)||score<0)hinge=false;
    else hinge=(dx[f]<0&&hit_z<0.5)||(dx[f]>0&&hit_z>0.5)||(dz[f]<0&&hit_x>0.5)||(dz[f]>0&&hit_x<0.5);
    *lower=mc_block_state_with_property(state,"hinge",hinge?"right":"left");return *lower<0?-1:0;
}
int mc_door_neighbor_change(mc_block_neighbor_read_t read,void *ctx,int x,int y,int z,
                            int32_t state,int changed_y,int32_t *result,bool *drop){
    *result=state;*drop=false;
    if(!mc_door_is_state(state))return 0;
    bool lower=mc_hinged_property(state,"half","lower");
    int32_t neighbor;int rc=read(ctx,x,changed_y,z,&neighbor);if(rc)return rc;
    if(changed_y==y+(lower?1:-1)){
        if(mc_door_is_state(neighbor)&&mc_hinged_property(neighbor,"half",lower?"upper":"lower"))
            *result=mc_block_state_with_property(neighbor,"half",lower?"lower":"upper");
        else *result=(int32_t)mc_block_desc_by_name("minecraft:air")->default_state;
    }else if(lower&&changed_y==y-1&&!(mc_hinged_support_flags(neighbor)&MC_SUPPORT_TOP))
        *result=(int32_t)mc_block_desc_by_name("minecraft:air")->default_state;
    *drop=lower&&*result!=state&&!mc_door_is_state(*result);return *result<0?-1:0;
}
bool mc_hinged_bounds(int32_t state,double min[3],double max[3]){
    const mc_hinged_family_t *f=mc_hinged_family(state);if(!f)return false;
    int d=direction(state);if(d<0)return false;
    for(int i=0;i<3;i++){min[i]=0;max[i]=1;}
    bool open=mc_hinged_property(state,"open","true");
    if(f->kind==2&&!open){if(mc_hinged_property(state,"half","top"))min[1]=13.0/16;else max[1]=3.0/16;return true;}
    if(f->kind==1&&open)d=(d+(mc_hinged_property(state,"hinge","right")?3:1))&3;
    if(d==0)min[2]=13.0/16;
    if(d==1)max[0]=3.0/16;
    if(d==2)max[2]=3.0/16;
    if(d==3)min[0]=13.0/16;
    return true;
}
bool mc_hinged_intersects(int32_t state,const double min[3],const double max[3]){
    double a[3],b[3];if(!mc_hinged_bounds(state,a,b))return false;
    for(int i=0;i<3;i++)if(min[i]>=b[i]-1e-6||max[i]<=a[i]+1e-6)return false;
    return true;
}
