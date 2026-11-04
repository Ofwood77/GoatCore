#define _GNU_SOURCE
#include "goatworld_store.h"
#include "../experimental/g1_internal.h"
#include "mc_anvil.h"
#include "mc_atomic_file.h"
#include "mc_util.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#define READERS 4
struct mc_goatworld {
    g1_store_t writer, readers[READERS];
    pthread_mutex_t writer_lock, reader_lock[READERS];
    unsigned initialized;
    _Atomic unsigned next_reader;
    bool failed;
    unsigned checkpoint_pages, last_attempt;
    int frames;
};
static int exec(g1_store_t *s,const char *sql){
    int r=sqlite3_exec(s->sql,sql,NULL,NULL,NULL);
    if(r!=SQLITE_OK){log_error("GoatWorld SQL rc=%d message=%s",r,sqlite3_errmsg(s->sql));errno=EIO;return -1;}
    return 0;
}
static int checkpoint(mc_goatworld_t *s,int mode){
    G1_HOOK("checkpoint-before");
    int pages=-1,done=-1;
    mc_save_clock_t span=mc_save_profile_begin();
    int r=sqlite3_wal_checkpoint_v2(s->writer.sql,NULL,mode,&pages,&done);
    mc_save_profile_end("sqlite_checkpoint",pages,done,span,mode,r==SQLITE_OK?0:-1);
    G1_HOOK("checkpoint-after");
    if(r!=SQLITE_OK){log_error("GoatWorld checkpoint failed rc=%d pages=%d done=%d: %s",r,pages,done,sqlite3_errmsg(s->writer.sql));errno=r==SQLITE_BUSY?EBUSY:EIO;return -1;}
    return 0;
}
static int wal_hook(void *context,sqlite3 *db,const char *name,int frames){
    (void)db;(void)name;
    mc_goatworld_t *s=context;s->frames=frames;return SQLITE_OK;
}
int mc_goatworld_open(const char *world,bool bound,mc_goatworld_t **out){
    *out=NULL;
    /* Reader connections and checkpoints may overlap: require upstream WAL-reset fix. */
    if(sqlite3_libversion_number()<3051003||!sqlite3_threadsafe()){errno=ENOTSUP;return -1;}
    char path[4096];
    if(snprintf(path,sizeof(path),"%s/goatworld",world)>=(int)sizeof(path)){errno=ENAMETOOLONG;return -1;}
    bool create=false;
    struct stat st;
    if(lstat(path,&st)){
        if(errno!=ENOENT||bound)return -1;
        if(mkdir(path,0700))return -1;
        create=true;
    }else if(!S_ISDIR(st.st_mode)){errno=EINVAL;return -1;}
    mc_goatworld_t *s=calloc(1,sizeof(*s));
    if(!s)return -1;
    s->writer.root=-1;
    for(unsigned i=0;i<READERS;i++)s->readers[i].root=-1;
    int r=pthread_mutex_init(&s->writer_lock,NULL);
    if(r){free(s);errno=r;return -1;}
    for(unsigned i=0;i<READERS;i++){
        r=pthread_mutex_init(&s->reader_lock[i],NULL);
        if(r){errno=r;goto bad;}s->initialized++;
    }
    s->writer.root=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(s->writer.root<0)goto bad;
    s->writer.writable=true;s->writer.runtime=true;
    int fd=openat(s->writer.root,"identity",(create?O_CREAT|O_EXCL|O_WRONLY:O_RDONLY)|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0)goto bad;
    int rc=0;
    if(create){
        size_t at=0;
        while(at<16){ssize_t n=getrandom(s->writer.id+at,16-at,0);if(n<0&&errno==EINTR)continue;if(n<=0){rc=-1;break;}at+=(size_t)n;}
        if(!rc&&(mc_file_write_all(fd,s->writer.id,16)||g1_sync(fd)))rc=-1;
    }else if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_nlink!=1||st.st_size!=16||mc_file_pread_all(fd,s->writer.id,16,0))rc=-1;
    int saved=errno;if(close(fd)&&!rc){rc=-1;saved=errno;}errno=saved;
    if(rc)goto bad;
    if(g1_sql_open(&s->writer,create)||g1_sql_check(&s->writer))goto bad;
    if(exec(&s->writer,"PRAGMA journal_size_limit=16777216;"))goto bad;
    s->checkpoint_pages=4096;
    const char *value=getenv("MC_GOATWORLD_CHECKPOINT_PAGES");
    if(value&&*value){char *end=NULL;errno=0;unsigned long v=strtoul(value,&end,10);if(errno||!end||*end||v<256||v>16384){errno=EINVAL;goto bad;}s->checkpoint_pages=(unsigned)v;}
    sqlite3_wal_hook(s->writer.sql,wal_hook,s);
    for(unsigned i=0;i<READERS;i++){
        s->readers[i].root=s->writer.root;
        memcpy(s->readers[i].id,s->writer.id,16);
        s->readers[i].runtime=true;
        if(g1_sql_open(&s->readers[i],false)||exec(&s->readers[i],"PRAGMA cache_size=-1024; PRAGMA query_only=ON;"))goto bad;
    }
    if(create){
        int parent=open(world,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if(parent<0)goto bad;
        rc=g1_sync(s->writer.root)||g1_sync(parent);saved=errno;
        if(close(parent)&&!rc){rc=-1;saved=errno;}errno=saved;
        if(rc)goto bad;
    }
    log_info("GoatWorld SQLite %s WAL FULL world=%s readers=%d checkpoint_pages=%u codec=LZ4",sqlite3_libversion(),world,READERS,s->checkpoint_pages);
    *out=s;return 0;
bad:;
    int error=errno;
    /* No checkpoint or creation retry on an incomplete/incompatible database. */
    for(unsigned i=0;i<READERS;i++)g1_sql_close(&s->readers[i]);
    g1_sql_close(&s->writer);
    if(s->writer.root>=0)close(s->writer.root);
    for(unsigned i=0;i<s->initialized;i++)pthread_mutex_destroy(&s->reader_lock[i]);
    pthread_mutex_destroy(&s->writer_lock);free(s);errno=error;return -1;
}
int mc_goatworld_read(mc_goatworld_t *s,int32_t x,int32_t z,mc_chunk_t *out){
    unsigned slot=atomic_fetch_add_explicit(&s->next_reader,1,memory_order_relaxed)%READERS;
    uint8_t *raw=NULL;size_t n=0;uint64_t gen=0;
    mc_save_clock_t span=mc_save_profile_begin();
    pthread_mutex_lock(&s->reader_lock[slot]);
    int rc=g1_sql_read(&s->readers[slot],0,x,z,&raw,&n,&gen);
    pthread_mutex_unlock(&s->reader_lock[slot]);
    mc_save_profile_end("sqlite_read",x,z,span,n,rc);
    if(rc)return rc;
    span=mc_save_profile_begin();
    rc=mc_anvil_decode_native(raw,n,x,z,out);
    mc_save_profile_end("sqlite_decode",x,z,span,n,rc);
    if(!rc){out->storage_nbt=raw;out->storage_nbt_len=n;out->save_generation=out->durable_generation=gen;raw=NULL;}
    free(raw);return rc;
}
int mc_goatworld_write(mc_goatworld_t *s,mc_chunk_t *const *chunks,size_t count){
    if(!chunks||!count||count>64){errno=EINVAL;return -1;}
    uint8_t *raw[64]={0};g1_update_t writes[64]={0};size_t total=0;
    int rc=-1,saved=0;
    for(size_t i=0;i<count;i++){
        if(!chunks[i]||!chunks[i]->save_generation||chunks[i]->save_generation>INT64_MAX){errno=EINVAL;goto done;}
        for(size_t j=0;j<i;j++)if(chunks[i]->cx==chunks[j]->cx&&chunks[i]->cz==chunks[j]->cz){errno=EINVAL;goto done;}
        size_t n=0;mc_save_clock_t span=mc_save_profile_begin();
        if(mc_anvil_encode_native(chunks[i],NULL,&raw[i],&n))goto done;
        mc_save_profile_end("sqlite_encode",chunks[i]->cx,chunks[i]->cz,span,n,0);
        if(n>G1_MAX_RAW||n>64u*1024u*1024u-total){errno=EFBIG;goto done;}total+=n;
        writes[i]=(g1_update_t){.x=chunks[i]->cx,.z=chunks[i]->cz,.raw=raw[i],.length=n,.generation=chunks[i]->save_generation};
    }
    G1_HOOK("prepare");
    pthread_mutex_lock(&s->writer_lock);
    if(s->failed){errno=EIO;rc=-1;}
    else if(s->frames>=32768 && checkpoint(s,SQLITE_CHECKPOINT_TRUNCATE)){
        /* Hard WAL backpressure, timeout zero: never let pinned readers grow an
         * unlimited journal. S4 retains dirty snapshots and reports the error. */
        rc=-1;
    }else{
        /* The same serialized connection is handed from startup to the S4 writer;
         * explicit save barriers/shutdown use it only after the queue is drained. */
        mc_save_clock_t span=mc_save_profile_begin();
        rc=g1_sql_write(&s->writer,0,writes,count,(g1_profile_t){2,0,0});
        mc_save_profile_end("sqlite_transaction",(int32_t)count,s->frames,span,total,rc);
        if(!rc){
            if((unsigned)s->frames<s->last_attempt)s->last_attempt=0;
            if((unsigned)s->frames>=s->checkpoint_pages && (unsigned)s->frames-s->last_attempt>=s->checkpoint_pages){
                s->last_attempt=(unsigned)s->frames;
                if(checkpoint(s,SQLITE_CHECKPOINT_PASSIVE))rc=G1_UNCERTAIN;
            }
        }
        if(rc&&errno!=ESTALE&&errno!=EBUSY)s->failed=true;
    }
    saved=errno;
    pthread_mutex_unlock(&s->writer_lock);
    errno=saved;
    if(!rc||rc==G1_UNCERTAIN){
        for(size_t i=0;i<count;i++){
            free(chunks[i]->storage_nbt);chunks[i]->storage_nbt=raw[i];chunks[i]->storage_nbt_len=writes[i].length;raw[i]=NULL;
            if(!rc)chunks[i]->durable_generation=chunks[i]->save_generation;
        }
    }
done:
    saved=errno;for(size_t i=0;i<count;i++)free(raw[i]);errno=saved;return rc;
}
int mc_goatworld_close(mc_goatworld_t *s){
    if(!s)return 0;
    G1_HOOK("close-before");
    int rc=s->failed?-1:0;
    /* All chunk readers and the S4 writer have been joined by world_destroy. */
    for(unsigned i=0;i<READERS;i++)if(g1_sql_close(&s->readers[i]))rc=-1;
    if(checkpoint(s,SQLITE_CHECKPOINT_TRUNCATE)||g1_sync(s->writer.root))rc=-1;
    if(g1_sql_close(&s->writer))rc=-1;
    if(close(s->writer.root))rc=-1;
    for(unsigned i=0;i<READERS;i++)pthread_mutex_destroy(&s->reader_lock[i]);
    pthread_mutex_destroy(&s->writer_lock);free(s);G1_HOOK("close-after");return rc;
}
