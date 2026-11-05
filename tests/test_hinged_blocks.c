#define _GNU_SOURCE
#include "mc_hinged_blocks.h"
#include "mc_hinged_world.h"
#include "mc_block_state.h"
#include "mc_block_drops.h"
#include "mc_util.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)
static long fail_after=-1;
void *__real_malloc(size_t n);void *__real_calloc(size_t n,size_t s);
void *__wrap_malloc(size_t n){if(fail_after>=0&&!fail_after--){fail_after=-1;return NULL;}return __real_malloc(n);}
void *__wrap_calloc(size_t n,size_t s){if(fail_after>=0&&!fail_after--){fail_after=-1;return NULL;}return __real_calloc(n,s);}
static int32_t state(const char *key){int32_t s=(int32_t)mc_global_state_id_from_key(key,UINT32_MAX);CHECK(s>=0);return s;}
static void geometry(void){
    FILE *f=fopen("tests/fixtures/vanilla-26.1.1-hinged-shapes.tsv","r");CHECK(f);
    char line[256];unsigned poses=0,families=0;
    while(fgets(line,sizeof(line),f)){
        int id,a[6],kind,manual,item,opened,closed;char name[100];
        if(sscanf(line,"F\t%99s\t%d\t%d\t%d\t%d\t%d",name,&kind,&manual,&item,&opened,&closed)==6){
            const mc_block_desc_t *b=mc_block_desc_by_name(name);CHECK(b);
            const mc_hinged_family_t *family=mc_hinged_family((int32_t)b->default_state);CHECK(family);
            CHECK(family->kind==kind&&family->manual==manual&&family->item==item&&family->open_sound==opened&&family->close_sound==closed);families++;
            if(kind==1){
                int32_t lower=mc_block_state_with_property((int32_t)b->default_state,"half","lower");
                int32_t upper=mc_block_state_with_property(lower,"half","upper");mc_block_drop_t drop;
                CHECK(mc_block_drop_resolve_default(lower,true,0,64,0,0,&drop));CHECK(drop.item_id==item&&drop.count==1);
                CHECK(!mc_block_drop_resolve_default(upper,true,0,65,0,0,&drop));
                CHECK(!mc_block_drop_resolve_default(lower,false,0,64,0,0,&drop));
            }
        }else if(sscanf(line,"H\t%d\t%d\t%d\t%d\t%d\t%d\t%d",&id,&a[0],&a[1],&a[2],&a[3],&a[4],&a[5])==7){
            double min[3],max[3];CHECK(mc_hinged_bounds(id,min,max));
            for(int i=0;i<3;i++)CHECK(min[i]*16==a[i]&&max[i]*16==a[i+3]);
            double center_min[3],center_max[3];for(int i=0;i<3;i++){center_min[i]=(min[i]+max[i])/2-0.01;center_max[i]=center_min[i]+0.02;}
            CHECK(mc_hinged_intersects(id,center_min,center_max));poses++;
        }
    }
    CHECK(!fclose(f));CHECK(poses==2688&&families==42);
    CHECK(mc_hinged_support_flags(state("minecraft:stone"))&MC_SUPPORT_TOP);
    CHECK(!(mc_hinged_support_flags(state("minecraft:oak_slab[type=bottom,waterlogged=false]"))&MC_SUPPORT_TOP));
    CHECK(mc_hinged_support_flags(state("minecraft:oak_slab[type=top,waterlogged=false]"))&MC_SUPPORT_TOP);
    CHECK(mc_hinged_support_flags(state("minecraft:oak_stairs[facing=north,half=top,shape=straight,waterlogged=false]"))&MC_SUPPORT_TOP);
    CHECK(!(mc_hinged_support_flags(state("minecraft:oak_stairs[facing=north,half=bottom,shape=straight,waterlogged=false]"))&MC_SUPPORT_TOP));
}
static void atomic_pair(void){
    int32_t air=state("minecraft:air"),door=state("minecraft:oak_door[facing=south,half=lower,hinge=left,open=false,powered=false]");
    int32_t upper=mc_block_state_with_property(door,"half","upper");mc_chunk_t c;
    CHECK(!mc_chunk_init(&c,0,0,(uint32_t)air));
    for(int fail=0;fail<2;fail++){
        uint64_t rev=c.revision,gen=c.save_generation;
        fail_after=fail;int rc=mc_chunk_set_column_pair(&c,8,63,8,(uint32_t)door,(uint32_t)upper);fail_after=-1;
        CHECK(rc<0);CHECK(mc_chunk_get_block(&c,8,63,8)==(uint32_t)air&&mc_chunk_get_block(&c,8,64,8)==(uint32_t)air);
        CHECK(c.revision==rev&&c.save_generation==gen);
    }
    c.save_pending=true;c.inflight_generation=0;
    CHECK(!mc_chunk_set_column_pair(&c,8,63,8,(uint32_t)door,(uint32_t)upper));CHECK(c.save_pending&&c.inflight_generation==0);
    CHECK(c.save_generation==2&&c.revision==2);
    uint64_t gen=c.save_generation;c.revision=UINT64_MAX-1;
    CHECK(mc_chunk_set_column_pair(&c,8,63,8,(uint32_t)air,(uint32_t)air)<0);
    CHECK(c.revision==UINT64_MAX-1&&c.save_generation==gen&&mc_chunk_get_block(&c,8,63,8)==(uint32_t)door);
    CHECK(mc_chunk_set_column_pair(&c,8,319,8,(uint32_t)door,(uint32_t)upper)<0);
    mc_chunk_destroy(&c);
}
static void load(mc_world_t *w){
    for(int z=-1;z<=1;z++)for(int x=-1;x<=1;x++)(void)mc_world_get_chunk(w,x,z,0);
    for(int tick=0;tick<1000;tick++){
        mc_world_tick(w,tick*50);bool ready=true;
        for(int z=-1;z<=1;z++)for(int x=-1;x<=1;x++)if(!mc_world_get_chunk(w,x,z,0))ready=false;
        if(ready)return;
        usleep(1000);
    }CHECK(false);
}
static void world(mc_storage_kind_t kind){
    char path[]="/tmp/goatcore-hinged.XXXXXX";CHECK(mkdtemp(path));
    mc_world_t *w=mc_world_create_with_storage(path,0,1,kind);CHECK(w);
    CHECK(!mc_world_configure_saves(w,kind==MC_STORAGE_BIN?MC_SAVE_IMMEDIATE:MC_SAVE_ASYNC));load(w);
    int32_t floor=state("minecraft:stone"),air=state("minecraft:air"),initial=state("minecraft:oak_door[facing=south,half=lower,hinge=left,open=false,powered=false]");
    CHECK(!mc_world_set_block(w,8,63,8,floor));
    int32_t lower;CHECK(!mc_hinged_prepare(w,8,64,8,initial,0,1,0.75,1,0.5,false,&lower));
    CHECK(mc_hinged_property(lower,"hinge","left"));CHECK(!mc_hinged_apply(w,8,64,8,lower));
    int32_t upper;CHECK(!mc_world_get_block_ready(w,8,65,8,&upper));CHECK(mc_hinged_property(upper,"half","upper"));
    CHECK(mc_hinged_interact(w,8,65,8,upper)==1);
    CHECK(!mc_world_get_block_ready(w,8,64,8,&lower));CHECK(mc_hinged_property(lower,"open","true"));
    CHECK(!mc_world_set_block(w,8,63,8,air));
    CHECK(!mc_world_get_block_ready(w,8,64,8,&lower)&&lower==air);
    CHECK(!mc_world_get_block_ready(w,8,65,8,&upper)&&upper==air);
    size_t count;const mc_environment_drop_t *drops=mc_world_environment_drops(w,&count);CHECK(count==1);
    mc_block_drop_t loot;CHECK(mc_block_drop_resolve_default(drops[0].state_id,true,8,64,8,0,&loot));CHECK(loot.count==1);
    mc_world_consume_environment_drops(w,1);CHECK(mc_world_environment_drops(w,&count)&&count==0);
    CHECK(!mc_world_set_block(w,8,63,8,floor));CHECK(!mc_hinged_apply(w,8,64,8,initial));
    int32_t loot_state;CHECK(mc_hinged_destroy(w,8,65,8,mc_block_state_with_property(initial,"half","upper"),&loot_state)==1);
    CHECK(loot_state==initial);(void)mc_world_environment_drops(w,&count);CHECK(count==0);
    CHECK(!mc_hinged_apply(w,8,64,8,initial));CHECK(!mc_world_flush_saves(w));
    if(kind==MC_STORAGE_BIN){
        CHECK(!mc_world_set_block(w,7,-1,7,floor));
        mc_chunk_t *c=mc_world_get_chunk(w,0,0,0);
        for(int y=0;y<320;y++)CHECK(!mc_chunk_set_block(c,7,y,7,(uint32_t)initial));
        CHECK(!mc_world_set_block(w,7,-1,7,air));
        (void)mc_world_environment_drops(w,&count);CHECK(count==320);
        mc_world_consume_environment_drops(w,count);
        for(int y=0;y<320;y++)CHECK(mc_chunk_get_block(c,7,y,7)==(uint32_t)air);
    }
    for(int family=0;family<2;family++){
        int32_t closed=family?state("minecraft:oak_trapdoor[facing=east,half=top,open=false,powered=true,waterlogged=true]"):initial;
        int y=family?70:64;CHECK(!mc_world_set_block(w,8,y,8,closed));
        mc_chunk_t *live=mc_world_get_chunk(w,0,0,0),snapshot;size_t bytes;
        CHECK(!mc_chunk_save_snapshot(live,&snapshot,64u*1024u*1024u,&bytes));
        live->save_pending=true;live->inflight_generation=snapshot.save_generation;
        CHECK(mc_hinged_interact(w,8,y,8,closed)==1);
        CHECK(!mc_chunk_complete_save(live,&snapshot,0));mc_chunk_destroy(&snapshot);
        CHECK(live->dirty&&live->save_generation>live->durable_generation);
        int32_t value;CHECK(!mc_world_get_block_ready(w,8,y,8,&value));CHECK(mc_hinged_property(value,"open","true"));
        if(family)CHECK(mc_hinged_property(value,"powered","true")&&mc_hinged_property(value,"waterlogged","true"));
    }
    CHECK(!mc_world_destroy_checked(w));w=mc_world_create_with_storage(path,0,1,kind);CHECK(w);load(w);
    for(int y=64;y<=70;y+=6){int32_t value;CHECK(!mc_world_get_block_ready(w,8,y,8,&value));CHECK(mc_hinged_property(value,"open","true"));}
    CHECK(!mc_world_destroy_checked(w));
}
int main(void){geometry();atomic_pair();world(MC_STORAGE_BIN);world(MC_STORAGE_ANVIL_ATOMIC);world(MC_STORAGE_GOATWORLD_SQLITE);
    printf("hinged blocks: %u checks; 2688 official shapes, atomic allocation failures, three stores and stale completion passed\n",checks);return 0;}
