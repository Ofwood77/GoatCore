#include "mc_attached_blocks.h"
#include "mc_block_state.h"
#include <limits.h>
#include <math.h>
#include <string.h>
struct box_set {uint8_t count,boxes[2][6];};
struct attached_shape {int32_t state;int light;struct box_set shape[2];};
#include "generated_attached_data.h"
static const int step[6][3]={{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};
static const char *direction_name[]={"down","up","north","south","west","east"};
bool mc_support_face(int32_t state,int face,mc_face_support_t type){
    return state>=0&&state<29873&&face>=0&&face<6&&type>=MC_FACE_FULL&&type<=MC_FACE_RIGID&&
        (face_support[state]&(1u<<(face+6*type)))!=0;
}
const mc_attached_family_t *mc_attached_family(int32_t state){
    const mc_block_desc_t *b=mc_block_state_desc(state);if(!b)return NULL;
    size_t lo=0,hi=sizeof(attached_families)/sizeof(*attached_families);
    while(lo<hi){size_t mid=lo+(hi-lo)/2;int c=strcmp(b->name,attached_families[mid].name);
        if(!c)return &attached_families[mid];
        if(c<0)hi=mid;else lo=mid+1;}
    return NULL;
}
static bool property(int32_t state,const char *key,const char *value){
    char text[24];return mc_block_state_property(state,key,text,sizeof(text))&&!strcmp(text,value);
}
static int facing(int32_t state){for(int i=2;i<6;i++)if(property(state,"facing",direction_name[i]))return i;return -1;}
/* Java's narrowing double->long saturates, rather than invoking C UB. */
static float trig(float radians,bool cosine){
    double index=(double)radians*10430.378350470453+(cosine?16384.0:0.0);
    int64_t n=index>=0x1p63?INT64_MAX:index<=-0x1p63?INT64_MIN:(int64_t)index;
    uint32_t bits=look_sin_bits[(uint64_t)n&65535u];float value;memcpy(&value,&bits,sizeof(value));return value;
}
int mc_placement_directions(float yaw,float pitch,int face,bool replacing,int order[6]){
    if(!order||!isfinite(yaw)||!isfinite(pitch)||face<0||face>5)return -1;
    float a=pitch*0.017453292f,b=-yaw*0.017453292f;
    float s=trig(a,false),c=trig(a,true),sy=trig(b,false),cy=trig(b,true);
    bool xp=sy>0,yp=s<0,zp=cy>0;
    float ax=xp?sy:-sy,ay=yp?-s:s,az=zp?cy:-cy,hx=ax*c,hz=az*c;
    int X=xp?5:4,Y=yp?1:0,Z=zp?3:2;
    if(ax>az){
        if(ay>hx){order[0]=Y;order[1]=X;order[2]=Z;}
        else if(hz>ay){order[0]=X;order[1]=Z;order[2]=Y;}
        else {order[0]=X;order[1]=Y;order[2]=Z;}
    }else{
        if(ay>hz){order[0]=Y;order[1]=Z;order[2]=X;}
        else if(hx>ay){order[0]=Z;order[1]=X;order[2]=Y;}
        else {order[0]=Z;order[1]=Y;order[2]=X;}
    }
    for(int i=0;i<3;i++)order[5-i]=order[i]^1;
    if(!replacing){int i=0;while(order[i]!=(face^1))i++;for(;i>0;i--)order[i]=order[i-1];order[0]=face^1;}
    return 0;
}
int mc_attached_support_direction(int32_t state){
    const mc_attached_family_t *f=mc_attached_family(state);if(!f)return -1;
    if(f->kind==1)return 0;
    if(f->kind==3)return property(state,"hanging","true")?1:0;
    int d=facing(state);return d<0?-1:d^1;
}
int mc_attached_survives(mc_block_neighbor_read_t read,void *ctx,int x,int y,int z,int32_t state,bool *result){
    const mc_attached_family_t *f=mc_attached_family(state);int d=mc_attached_support_direction(state);
    if(!f||!read||!result||d<0||x<=INT32_MIN||x>=INT32_MAX||z<=INT32_MIN||z>=INT32_MAX||y<=INT32_MIN||y>=INT32_MAX)return -1;
    int32_t support;int rc=read(ctx,x+step[d][0],y+step[d][1],z+step[d][2],&support);if(rc)return rc;
    *result=mc_support_face(support,d^1,(f->kind==1||f->kind==3)?MC_FACE_CENTER:MC_FACE_FULL);return 0;
}
static int horizontal(mc_block_neighbor_read_t read,void *ctx,int x,int y,int z,int32_t state,const int order[6],int32_t *result){
    for(int i=0;i<6;i++)if(order[i]>=2){
        int32_t candidate=mc_block_state_with_property(state,"facing",direction_name[order[i]^1]);
        bool valid;int rc=mc_attached_survives(read,ctx,x,y,z,candidate,&valid);if(rc)return rc;
        if(valid){*result=candidate;return 0;}
    }
    return 2; /* no candidate; distinct from a reader error */
}
int mc_attached_placement(mc_block_neighbor_read_t read,void *ctx,int x,int y,int z,int32_t state,float yaw,float pitch,int face,bool replacing,int32_t *result){
    const mc_attached_family_t *f=mc_attached_family(state);int order[6];
    if(!f||!read||!result||y< -64||y>=320||mc_placement_directions(yaw,pitch,face,replacing,order))return -1;
    if(f->kind==4){
        if(!replacing){
            int32_t behind;int rc=read(ctx,x+step[face^1][0],y+step[face^1][1],z+step[face^1][2],&behind);if(rc)return rc;
            const mc_attached_family_t *old=mc_attached_family(behind);
            if(old&&old->kind==4&&facing(behind)==face)return -1;
        }
        int rc=horizontal(read,ctx,x,y,z,state,order,result);return rc==2?-1:rc;
    }
    if(f->kind==3){
        for(int i=0;i<6;i++)if(order[i]<2){
            int32_t candidate=mc_block_state_with_property(state,"hanging",order[i]==1?"true":"false");
            bool valid;int rc=mc_attached_survives(read,ctx,x,y,z,candidate,&valid);if(rc)return rc;
            if(valid){*result=candidate;return 0;}
        }
        return -1;
    }
    const mc_attached_family_t *standing=NULL,*wall=NULL;
    for(size_t i=0;i<sizeof(attached_families)/sizeof(*attached_families);i++){
        const mc_attached_family_t *a=&attached_families[i];if(a->item!=f->item)continue;
        if(a->kind==1)standing=a;else if(a->kind==2)wall=a;
    }
    if(!standing||!wall)return -1;
    int32_t wall_state=-1;int wall_rc=horizontal(read,ctx,x,y,z,wall->default_state,order,&wall_state);
    /* StandingAndWallBlockItem computes the wall candidate before its loop. */
    if(wall_rc<0||wall_rc==1)return wall_rc;
    for(int i=0;i<6;i++){
        if(order[i]==1)continue;
        int32_t candidate=order[i]==0?standing->default_state:wall_state;
        if(candidate<0)continue;
        bool valid;int rc=mc_attached_survives(read,ctx,x,y,z,candidate,&valid);if(rc)return rc;
        if(valid){*result=candidate;return 0;}
    }
    return -1;
}
static const struct attached_shape *shape(int32_t state){
    size_t lo=0,hi=sizeof(attached_shapes)/sizeof(*attached_shapes);
    while(lo<hi){size_t m=lo+(hi-lo)/2;if(state==attached_shapes[m].state)return &attached_shapes[m];
        if(state<attached_shapes[m].state)hi=m;else lo=m+1;}
    return NULL;
}
int mc_attached_boxes(int32_t state,bool collision,mc_block_box_t boxes[2]){
    const struct attached_shape *s=shape(state);if(!s||!boxes)return -1;
    const struct box_set *b=&s->shape[collision?1:0];
    for(unsigned n=0;n<b->count;n++)for(int d=0;d<3;d++){
        boxes[n].min[d]=b->boxes[n][d]/32.0;boxes[n].max[d]=b->boxes[n][d+3]/32.0;
    }
    return b->count;
}
bool mc_attached_intersects(int32_t state,const double min[3],const double max[3]){
    mc_block_box_t boxes[2];int n=mc_attached_boxes(state,true,boxes);
    for(int i=0;i<n;i++){
        bool hit=true;for(int d=0;d<3;d++)if(min[d]>=boxes[i].max[d]-1e-6||max[d]<=boxes[i].min[d]+1e-6)hit=false;
        if(hit)return true;
    }
    return false;
}
int mc_attached_light(int32_t state){const struct attached_shape *s=shape(state);return s?s->light:-1;}
