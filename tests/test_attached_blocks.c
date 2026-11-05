#define _GNU_SOURCE
#include "mc_attached_blocks.h"
#include "mc_attached_world.h"
#include "mc_hinged_blocks.h"
#include "mc_mining.h"
#include "mc_block_state.h"
#include "mc_block_drops.h"
#include "mc_world.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static unsigned checks;
static bool fail_realloc;
void *__real_realloc(void *p,size_t n);
void *__wrap_realloc(void *p,size_t n){if(fail_realloc){fail_realloc=false;return NULL;}return __real_realloc(p,n);}

#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)
static int32_t state(const char *key){int32_t s=(int32_t)mc_global_state_id_from_key(key,UINT32_MAX);CHECK(s>=0);return s;}
static const int step[6][3]={{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};
static struct {int32_t value[6];int unavailable,error;} scene;
static int read_neighbor(void *ctx,int32_t x,int32_t y,int32_t z,int32_t *out){
    (void)ctx;for(int i=0;i<6;i++)if(x==step[i][0]&&y==64+step[i][1]&&z==step[i][2]){
        if(scene.unavailable&(1<<i))return 1;
        if(scene.error&(1<<i))return -1;
        *out=scene.value[i];return 0;
    }*out=state("minecraft:air");return 0;
}
static void official(void){
    FILE *f=fopen("tests/fixtures/vanilla-26.1.1-attached.tsv","r");CHECK(f);char line[512];unsigned supports=0,shapes=0,looks=0,families=0;
    while(fgets(line,sizeof(line),f)){
        int id,type,count;unsigned mask;
        if(sscanf(line,"S\t%d\t%u",&id,&mask)==2){
            for(int t=0;t<3;t++)for(int d=0;d<6;d++)CHECK(mc_support_face(id,d,(mc_face_support_t)t)==!!(mask&(1u<<(d+6*t))));
            supports++;
        }else if(line[0]=='H'){
            char *end=line+1;id=(int)strtol(end,&end,10);type=(int)strtol(end,&end,10);count=(int)strtol(end,&end,10);
            mc_block_box_t boxes[2];CHECK(mc_attached_boxes(id,type,boxes)==count);
            for(int i=0;i<count;i++)for(int n=0;n<6;n++){int v=(int)strtol(end,&end,10);CHECK((n<3?boxes[i].min[n]:boxes[i].max[n-3])*32==v);}
            shapes++;
        }else if(line[0]=='O'){
            float yaw,pitch;int want[6],got[6];CHECK(sscanf(line,"O\t%f\t%f\t%d\t%d\t%d\t%d\t%d\t%d",&yaw,&pitch,&want[0],&want[1],&want[2],&want[3],&want[4],&want[5])==8);
            CHECK(!mc_placement_directions(yaw,pitch,1,true,got));for(int n=0;n<6;n++){if(want[n]!=got[n])fprintf(stderr,"look yaw=%g pitch=%g i=%d want=%d got=%d\n",yaw,pitch,n,want[n],got[n]);CHECK(want[n]==got[n]);}
            for(int face=0;face<6;face++){CHECK(!mc_placement_directions(yaw,pitch,face,false,got));CHECK(got[0]==(face^1));unsigned seen=0;for(int n=0;n<6;n++)seen|=1u<<got[n];CHECK(seen==63);}
            looks++;
        }else if(line[0]=='F'){
            char name[100];int kind,item,def;CHECK(sscanf(line,"F\t%99s\t%d\t%d\t%d",name,&kind,&item,&def)==4);
            const mc_attached_family_t *family=mc_attached_family(def);CHECK(family&&family->kind==kind&&family->item==item&&!strcmp(family->name,name));
            const mc_block_desc_t *b=mc_block_state_desc(def);CHECK(b);
            for(uint32_t n=b->min_state_id;n<=b->max_state_id;n++){
                CHECK(mc_attached_family((int)n)==family);mc_block_drop_t drop;CHECK(mc_block_drop_resolve_default((int)n,true,0,64,0,0,&drop));CHECK(drop.item_id==item&&drop.count==1);
                CHECK(!mc_block_drop_resolve_default((int)n,false,0,64,0,0,&drop));
                int d=mc_attached_support_direction((int)n);CHECK(d>=0&&d<6);
                memset(&scene,0,sizeof(scene));scene.value[d]=state("minecraft:stone");bool valid=false;
                CHECK(!mc_attached_survives(read_neighbor,NULL,0,64,0,(int)n,&valid)&&valid);
                scene.value[d]=state("minecraft:air");CHECK(!mc_attached_survives(read_neighbor,NULL,0,64,0,(int)n,&valid)&&!valid);
                scene.unavailable=1<<d;CHECK(mc_attached_survives(read_neighbor,NULL,0,64,0,(int)n,&valid)==1);
                scene.unavailable=0;scene.error=1<<d;CHECK(mc_attached_survives(read_neighbor,NULL,0,64,0,(int)n,&valid)==-1);
                if(kind>=3){CHECK(mc_hinged_property(mc_block_placement_in_water((int)n,0),"waterlogged","true"));CHECK(mc_hinged_property(mc_block_placement_in_water((int)n,1),"waterlogged","false"));}
            }families++;
        }else if(sscanf(line,"B\t%d\t%d\t%d",&id,&type,&count)==3){
            mc_mining_break_info_t info=mc_mining_break_info(id,NULL);
            CHECK(info.known_hardness&&info.hardness_x100==type&&info.instant==(type==0));
            CHECK(info.requires_correct_tool==(count!=0));
        }else if(sscanf(line,"L\t%d\t%d",&id,&type)==2)CHECK(mc_attached_light(id)==type);
    }
    CHECK(!fclose(f));CHECK(supports==29873&&shapes==126&&looks==247&&families==17);
    int dirs[6];CHECK(mc_placement_directions(NAN,0,1,false,dirs)<0);CHECK(mc_placement_directions(0,INFINITY,1,false,dirs)<0);
    CHECK(!mc_support_face(-1,1,MC_FACE_CENTER));CHECK(!mc_support_face(1,6,MC_FACE_FULL));
    /* Standing-and-wall candidate preference and ladder-front refusal. */
    int32_t result;memset(&scene,0,sizeof(scene));scene.value[0]=state("minecraft:stone");
    CHECK(!mc_attached_placement(read_neighbor,NULL,0,64,0,state("minecraft:torch"),0,0,1,false,&result));CHECK(result==state("minecraft:torch"));
    memset(&scene,0,sizeof(scene));scene.value[4]=state("minecraft:stone");
    CHECK(!mc_attached_placement(read_neighbor,NULL,0,64,0,state("minecraft:torch"),0,0,5,false,&result));CHECK(mc_hinged_property(result,"facing","east"));
    scene.value[4]=state("minecraft:ladder[facing=east,waterlogged=false]");
    CHECK(mc_attached_placement(read_neighbor,NULL,0,64,0,state("minecraft:ladder[facing=north,waterlogged=false]"),0,0,5,false,&result)<0);
    memset(&scene,0,sizeof(scene));scene.unavailable=63;
    CHECK(mc_attached_placement(read_neighbor,NULL,0,64,0,state("minecraft:torch"),0,0,1,false,&result)==1);
}
static void load(mc_world_t *w){
    for(int z=-1;z<=1;z++)for(int x=-1;x<=1;x++)(void)mc_world_get_chunk(w,x,z,0);
    for(int tick=0;tick<2000;tick++){
        mc_world_tick(w,tick*50);bool ready=true;for(int z=-1;z<=1;z++)for(int x=-1;x<=1;x++)if(!mc_world_get_chunk(w,x,z,0))ready=false;
        if(ready)return;
        usleep(1000);
    }CHECK(false);
}
static void world(mc_storage_kind_t kind){
    char path[]="/tmp/goatcore-attached.XXXXXX";CHECK(mkdtemp(path));
    mc_world_t *w=mc_world_create_with_storage(path,0,1,kind);CHECK(w);
    CHECK(!mc_world_configure_saves(w,kind==MC_STORAGE_BIN?MC_SAVE_IMMEDIATE:MC_SAVE_ASYNC));load(w);
    int32_t stone=state("minecraft:stone"),air=state("minecraft:air");
    const char *keys[]={"minecraft:torch","minecraft:soul_torch","minecraft:copper_torch",
        "minecraft:wall_torch[facing=east]","minecraft:soul_wall_torch[facing=east]","minecraft:copper_wall_torch[facing=east]",
        "minecraft:lantern[hanging=false,waterlogged=false]","minecraft:lantern[hanging=true,waterlogged=true]",
        "minecraft:ladder[facing=east,waterlogged=true]"};
    /* The supporting cell at x=-1 is across both a chunk and region boundary. */
    for(unsigned i=0;i<sizeof(keys)/sizeof(*keys);i++){
        int32_t attached=state(keys[i]);int d=mc_attached_support_direction(attached);
        int sx=step[d][0],sy=80+step[d][1],sz=step[d][2];
        CHECK(!mc_world_set_block(w,sx,sy,sz,stone));CHECK(!mc_world_set_block(w,0,80,0,attached));
        CHECK(!mc_world_flush_saves(w));
        mc_chunk_t *live=mc_world_get_chunk(w,0,0,0),snapshot;size_t bytes;
        CHECK(!mc_chunk_save_snapshot(live,&snapshot,64u*1024u*1024u,&bytes));live->save_pending=true;live->inflight_generation=snapshot.save_generation;
        CHECK(!mc_world_set_block(w,sx,sy,sz,air));
        int32_t value;CHECK(!mc_world_get_block_ready(w,0,80,0,&value)&&value==(mc_hinged_property(attached,"waterlogged","true")?mc_world_ids(w)->water_level[0]:air));
        size_t count;const mc_environment_drop_t *drops=mc_world_environment_drops(w,&count);CHECK(count==1&&drops[0].state_id==attached);
        CHECK(!mc_world_set_block(w,sx,sy,sz,air));mc_world_environment_drops(w,&count);CHECK(count==1);
        CHECK(!mc_chunk_complete_save(live,&snapshot,0));mc_chunk_destroy(&snapshot);CHECK(live->dirty&&live->save_generation>live->durable_generation);
        mc_world_consume_environment_drops(w,count);CHECK(!mc_world_flush_saves(w));
    }
    /* Reserve the environmental drop before removing its block. An injected
     * allocation failure preserves it, and the next tick resumes the same stack. */
    if(kind==MC_STORAGE_BIN){
        mc_world_t *f=mc_world_create_with_workers(NULL,0,1);CHECK(f);load(f);
        CHECK(!mc_world_set_block(f,8,80,8,stone));CHECK(!mc_world_set_block(f,8,81,8,state("minecraft:torch")));
        fail_realloc=true;CHECK(mc_world_set_block(f,8,80,8,air)<0);CHECK(!fail_realloc);
        int32_t v;CHECK(!mc_world_get_block_ready(f,8,81,8,&v)&&v==state("minecraft:torch"));
        size_t n;mc_world_environment_drops(f,&n);CHECK(n==0);
        mc_world_tick(f,100);CHECK(!mc_world_get_block_ready(f,8,81,8,&v)&&v==air);
        mc_world_environment_drops(f,&n);CHECK(n==1);mc_world_tick(f,150);mc_world_environment_drops(f,&n);CHECK(n==1);
        CHECK(!mc_world_destroy_checked(f));
    }
    /* NORTH stairs gain a full WEST side in inner_left. Removing the
     * southern stair changes that support indirectly, diagonally to the torch. */
    int32_t stair=state("minecraft:oak_stairs[facing=north,half=bottom,shape=straight,waterlogged=false]");
    CHECK(!mc_world_set_block(w,8,110,8,stair));
    CHECK(!mc_world_set_block(w,8,110,9,mc_block_state_with_property(stair,"facing","west")));
    int32_t support_value;CHECK(!mc_world_get_block_ready(w,8,110,8,&support_value));CHECK(mc_support_face(support_value,4,MC_FACE_FULL));
    CHECK(!mc_world_set_block(w,7,110,8,state("minecraft:wall_torch[facing=west]")));
    CHECK(!mc_world_set_block(w,8,110,9,air));
    int32_t wall_value;CHECK(!mc_world_get_block_ready(w,7,110,8,&wall_value)&&wall_value==air);
    size_t shape_drops;mc_world_environment_drops(w,&shape_drops);CHECK(shape_drops==1);mc_world_consume_environment_drops(w,shape_drops);
    CHECK(!mc_world_set_block(w,8,110,8,air));
    /* Six attachments share one support. Repeated updates cannot double-drop. */
    CHECK(!mc_world_set_block(w,8,100,8,stone));
    const char *face_names[]={"down","up","north","south","west","east"};
    for(int d=0;d<6;d++){
        int32_t a=d==1?state("minecraft:torch"):d==0?state("minecraft:lantern[hanging=true,waterlogged=false]"):
            mc_block_state_with_property(state("minecraft:wall_torch[facing=north]"),"facing",face_names[d]);
        CHECK(!mc_world_set_block(w,8+step[d][0],100+step[d][1],8+step[d][2],a));
    }
    CHECK(!mc_world_set_block(w,8,100,8,air));size_t drops;mc_world_environment_drops(w,&drops);CHECK(drops==6);
    for(int d=0;d<6;d++){int32_t v;CHECK(!mc_world_get_block_ready(w,8+step[d][0],100+step[d][1],8+step[d][2],&v)&&v==air);}
    CHECK(!mc_world_set_block(w,8,100,8,stone));CHECK(!mc_world_set_block(w,8,100,8,air));mc_world_environment_drops(w,&drops);CHECK(drops==6);mc_world_consume_environment_drops(w,drops);
    CHECK(!mc_world_set_block(w,4,90,4,stone));CHECK(!mc_world_set_block(w,4,91,4,state("minecraft:torch")));
    CHECK(!mc_world_set_block(w,5,94,5,stone));
    int32_t wet_lantern=state("minecraft:lantern[hanging=false,waterlogged=true]");
    int32_t wet_ladder=state("minecraft:ladder[facing=east,waterlogged=true]");
    CHECK(!mc_world_set_block(w,5,95,5,wet_lantern));CHECK(!mc_world_set_block(w,6,95,6,stone));CHECK(!mc_world_set_block(w,7,95,6,wet_ladder));
    CHECK(!mc_world_destroy_checked(w));w=mc_world_create_with_storage(path,0,1,kind);CHECK(w);load(w);
    int32_t value;CHECK(!mc_world_get_block_ready(w,0,80,0,&value)&&value==mc_world_ids(w)->water_level[0]);CHECK(!mc_world_get_block_ready(w,4,91,4,&value)&&value==state("minecraft:torch"));
    CHECK(!mc_world_get_block_ready(w,5,95,5,&value)&&value==wet_lantern);CHECK(!mc_world_get_block_ready(w,7,95,6,&value)&&value==wet_ladder);
    CHECK(!mc_world_set_block(w,4,90,4,air));CHECK(!mc_world_get_block_ready(w,4,91,4,&value)&&value==air);
    CHECK(!mc_world_destroy_checked(w));char command[256];snprintf(command,sizeof(command),"rm -rf -- '%s'",path);CHECK(system(command)==0);
}
static void unavailable_world(void){
    mc_world_t *w=mc_world_create_with_workers(NULL,0,1);CHECK(w);
    mc_chunk_t *c=NULL;
    for(int i=0;i<1000&&!c;i++){mc_world_tick(w,i*50);c=mc_world_get_chunk(w,0,0,0);usleep(1000);}CHECK(c);
    int32_t ladder=state("minecraft:ladder[facing=east,waterlogged=false]"),air=state("minecraft:air");
    CHECK(!mc_world_set_block(w,0,80,8,ladder));
    /* Repeated notifications from a loaded neighbor never turn missing west
     * data into an air support. Neither the predicate nor the update loads it. */
    CHECK(!mc_world_set_block(w,1,80,8,state("minecraft:stone")));
    for(int i=0;i<5;i++)mc_world_tick(w,i*50);
    CHECK(mc_chunk_get_block(c,0,80,8)==(uint32_t)ladder);size_t count;mc_world_environment_drops(w,&count);CHECK(count==0);
    mc_chunk_t *west=NULL;
    for(int i=0;i<1000&&!west;i++){mc_world_tick(w,i*50);west=mc_world_get_chunk(w,-1,0,0);usleep(1000);}CHECK(west);
    CHECK(mc_chunk_get_block(west,15,80,8)==(uint32_t)air);
    CHECK(mc_chunk_get_block(c,0,80,8)==(uint32_t)air);mc_world_environment_drops(w,&count);CHECK(count==1);
    CHECK(!mc_world_destroy_checked(w));
}
int main(void){official();unavailable_world();world(MC_STORAGE_BIN);world(MC_STORAGE_ANVIL_ATOMIC);world(MC_STORAGE_GOATWORLD_SQLITE);printf("attached blocks: %u checks passed\n",checks);return 0;}
