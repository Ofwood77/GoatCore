#define _GNU_SOURCE
#include "mc_world.h"
#include "mc_save_pipeline.h"
#include "mc_anvil.h"
#include "mc_goatworld.h"
#include <sqlite3.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <time.h>
static unsigned checks;
#define CHECK(v) do{checks++;if(!(v)){fprintf(stderr,"line %d errno=%d: %s\n",__LINE__,errno,#v);abort();}}while(0)
static char *temporary(void){char *p=strdup("/tmp/goatcore-g2-unit.XXXXXX");CHECK(p&&mkdtemp(p));return p;}
static void sample(mc_chunk_t *c,int x,int z){CHECK(!mc_chunk_init(c,x,z,0));CHECK(!mc_chunk_set_block(c,3,64,2,1));}
struct reader {mc_world_storage_t *s;_Atomic bool stop;_Atomic unsigned reads;};
static void *reading(void *v){struct reader *r=v;while(!atomic_load(&r->stop)){
    mc_chunk_t c={0};int rc=mc_storage_read(r->s,-33,31,&c,NULL);assert(rc==0);assert(c.save_generation>=1);mc_chunk_destroy(&c);atomic_fetch_add(&r->reads,1);
}return NULL;}
static mc_chunk_t *ready_chunk(mc_world_t *w){
    mc_chunk_t *c=NULL;
    for(unsigned i=0;i<2000;i++){
        c=mc_world_get_chunk(w,-1,-1,0);mc_world_tick(w,i*50);
        if(c)return c;
        struct timespec pause={0,1000000};nanosleep(&pause,NULL);
    }
    CHECK(c);return c;
}
static void world_cycle(void){
    char *path=temporary();
    mc_world_t *w=mc_world_create_with_storage(path,0,1,MC_STORAGE_GOATWORLD_SQLITE);CHECK(w);
    CHECK(!mc_world_configure_saves(w,MC_SAVE_ASYNC));
    CHECK(ready_chunk(w));CHECK(!mc_world_set_block(w,-8,200,-8,1));CHECK(!mc_world_flush_saves(w));
    CHECK(mc_world_evict_outside(w,NULL,0,10)==1);
    mc_chunk_t *c=ready_chunk(w);CHECK(mc_chunk_get_block(c,8,200,8)==1);
    CHECK(!mc_world_set_block(w,-8,200,-8,2));mc_world_tick(w,200000);
    CHECK(!mc_world_destroy_checked(w));
    mc_world_storage_t *store=NULL;CHECK(!mc_storage_open(path,MC_STORAGE_GOATWORLD_SQLITE,&store));
    mc_chunk_t out={0};CHECK(!mc_storage_read(store,-1,-1,&out,NULL));CHECK(mc_chunk_get_block(&out,8,200,8)==2);
    CHECK(out.save_generation==out.durable_generation&&out.save_generation>0);mc_chunk_destroy(&out);
    CHECK(!mc_storage_close_checked(store));free(path);
}
static int command(int argc,char **argv){
    if(argc!=4||strcmp(argv[1],"--disposable"))return 2;
    char marker[4096];struct stat st;
    snprintf(marker,sizeof(marker),"%s/.goatcore-g2-disposable",argv[3]);
    if(lstat(marker,&st)||!S_ISREG(st.st_mode))return 2;
    if(!strcmp(argv[2],"world-update")){
        mc_world_t *w=mc_world_create_with_storage(argv[3],0,1,MC_STORAGE_GOATWORLD_SQLITE);
        if(!w||mc_world_configure_saves(w,MC_SAVE_ASYNC))return 1;
        const int xs[]={-33,32},zs[]={31,-33};
        for(unsigned at=0;at<2;at++){
            bool ready=false;
            for(unsigned round=0;round<2000;round++){
                mc_chunk_t *chunk=mc_world_get_chunk(w,xs[at],zs[at],0);mc_world_tick(w,round*50);
                if(chunk){ready=true;break;}
                struct timespec pause={0,1000000};nanosleep(&pause,NULL);
            }
            if(!ready){mc_world_destroy_checked(w);return 1;}
        }
        for(unsigned at=0;at<2;at++)if(mc_world_set_block(w,xs[at]*16+3,64,zs[at]*16+2,2)){mc_world_destroy_checked(w);return 1;}
        int flush=mc_world_flush_saves(w),shutdown=mc_world_destroy_checked(w);
        printf("{\"flush\":%d,\"shutdown\":%d}\n",flush,shutdown);
        return flush||shutdown?1:0;
    }
    mc_world_storage_t *store=NULL;
    if(mc_storage_open(argv[3],MC_STORAGE_GOATWORLD_SQLITE,&store))return 1;
    mc_chunk_t a={0},b={0};mc_chunk_t *items[]={&a,&b};
    int result=0;
    for(unsigned i=0;i<2;i++){
        int x=i?32:-33,z=i?-33:31;
        if(!strcmp(argv[2],"seed"))sample(items[i],x,z);
        else if(mc_storage_read(store,x,z,items[i],NULL)){result=1;break;}
        if(!strcmp(argv[2],"update")){
            if(mc_chunk_set_block(items[i],3,64,2,2)){result=1;break;}
        }
    }
    if(!result&&!strcmp(argv[2],"wal")){
        for(unsigned round=0;round<2000;round++){
            for(unsigned i=0;i<2;i++)if(mc_chunk_set_block(items[i],3,64,2,round%2?1:2))result=1;
            if(result||mc_storage_write_region(store,items,2)){result=1;break;}
        }
    }
    if(!result&&strcmp(argv[2],"read")&&strcmp(argv[2],"wal")){
        mc_save_batch_t *batch=calloc(1,sizeof(*batch));
        if(!batch)result=1;
        else{
            for(unsigned i=0;i<2;i++)if(mc_save_batch_add(batch,items[i]))result=1;
            if(!result){mc_save_batch_execute(store,batch);result=batch->results[0]||batch->results[1];}
            mc_save_batch_destroy(batch);
        }
    }
    if(!result)printf("{\"generations\":[%llu,%llu],\"states\":[%u,%u]}\n",(unsigned long long)a.save_generation,(unsigned long long)b.save_generation,mc_chunk_get_block(&a,3,64,2),mc_chunk_get_block(&b,3,64,2));
    mc_chunk_destroy(&a);mc_chunk_destroy(&b);
    if(mc_storage_close_checked(store))result=1;
    return result?1:0;
}
int main(int argc,char **argv){
    if(argc>1)return command(argc,argv);
    CHECK(sqlite3_libversion_number()>=3051003);
    world_cycle();
    char *world=temporary();mc_world_storage_t *s=NULL,*other=NULL;
    CHECK(!mc_storage_open(world,MC_STORAGE_GOATWORLD_SQLITE,&s));
    CHECK(mc_storage_open(world,MC_STORAGE_BIN,&other)<0&&!other);
    CHECK(mc_storage_open(world,MC_STORAGE_GOATWORLD_SQLITE,&other)<0&&!other);
    mc_chunk_t a={0},b={0},read={0};sample(&a,-33,31);sample(&b,32,-33);
    CHECK(mc_storage_read(s,-33,31,&read,NULL)==1);
    mc_save_batch_t *batch=calloc(1,sizeof(*batch));CHECK(batch);
    CHECK(!mc_save_batch_add(batch,&a));CHECK(!mc_save_batch_add(batch,&b));
    a.save_pending=b.save_pending=true;a.inflight_generation=a.save_generation;b.inflight_generation=b.save_generation;
    mc_save_batch_t *rejected=calloc(1,sizeof(*rejected));CHECK(rejected&&!mc_save_batch_add(rejected,&a));
    mc_save_io_t *io=NULL;CHECK(!mc_save_io_create(s,&io));CHECK(!mc_save_io_submit(io,batch));
    CHECK(mc_save_io_submit(io,rejected)<0&&errno==EAGAIN);mc_save_batch_destroy(rejected);
    CHECK(!mc_chunk_set_block(&a,3,64,2,2));
    batch=mc_save_io_take(io,true);CHECK(batch&&batch->results[0]==0&&batch->results[1]==0);
    CHECK(!mc_chunk_complete_save(&a,&batch->chunks[0],batch->results[0]));CHECK(!mc_chunk_complete_save(&b,&batch->chunks[1],batch->results[1]));mc_save_batch_destroy(batch);
    CHECK(!mc_storage_read(s,-33,31,&read,NULL));CHECK(read.save_generation==a.durable_generation&&a.save_generation>a.durable_generation&&a.dirty);mc_chunk_destroy(&read);
    struct reader reader={.s=s};pthread_t thread;CHECK(!pthread_create(&thread,NULL,reading,&reader));
    for(unsigned i=0;i<100;i++){
        CHECK(!mc_chunk_set_block(&a,3,64,2,(i%2)+1));CHECK(!mc_chunk_mark_dirty(&a));
        batch=calloc(1,sizeof(*batch));CHECK(batch&&!mc_save_batch_add(batch,&a));
        a.save_pending=true;a.inflight_generation=a.save_generation;
        CHECK(!mc_save_io_submit(io,batch));batch=mc_save_io_take(io,true);CHECK(batch&&batch->results[0]==0);
        CHECK(!mc_chunk_complete_save(&a,&batch->chunks[0],0));mc_save_batch_destroy(batch);
    }
    atomic_store(&reader.stop,true);CHECK(!pthread_join(thread,NULL));CHECK(atomic_load(&reader.reads)>0);
    uint64_t gen=a.save_generation;
    a.save_generation--;CHECK(mc_storage_write(s,&a,NULL)<0);a.save_generation=gen;
    CHECK(!mc_save_io_close(io,&batch)&&!batch);
    CHECK(!mc_storage_close_checked(s));s=NULL;
    CHECK(!mc_storage_open(world,MC_STORAGE_GOATWORLD_SQLITE,&s));
    CHECK(!mc_storage_read(s,-33,31,&read,NULL));CHECK(read.save_generation==gen);CHECK(mc_chunk_get_block(&read,3,64,2)==2);mc_chunk_destroy(&read);
    CHECK(!mc_storage_close_checked(s));
    char database[4096];snprintf(database,sizeof(database),"%s/goatworld/chunks.sqlite",world);
    sqlite3 *sql=NULL;CHECK(sqlite3_open(database,&sql)==SQLITE_OK);
    CHECK(sqlite3_exec(sql,"UPDATE chunks SET sha=zeroblob(32)",NULL,NULL,NULL)==SQLITE_OK);CHECK(sqlite3_close(sql)==SQLITE_OK);
    CHECK(!mc_storage_open(world,MC_STORAGE_GOATWORLD_SQLITE,&s));
    CHECK(mc_storage_read(s,-33,31,&read,NULL)<0);CHECK(!mc_storage_close_checked(s));
    CHECK(unlink(database)==0);CHECK(mc_storage_open(world,MC_STORAGE_GOATWORLD_SQLITE,&s)<0&&!s);
    CHECK(mc_storage_open(world,MC_STORAGE_BIN,&s)<0&&!s);
    mc_chunk_destroy(&a);mc_chunk_destroy(&b);
    printf("GoatWorld integrated: %u checks passed; fixture=%s\n",checks,world);free(world);return 0;
}
