"""Real preload probe regression: exact counters, errno and concurrent writes."""
import csv
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class IOProbeTest(unittest.TestCase):
    def test_world_filter_counts_and_errno(self):
        with tempfile.TemporaryDirectory(prefix='goatcore-s5-probe-') as tmp:
            folder = Path(tmp)
            world = folder/'world';world.mkdir()
            library = folder/'probe.so'
            subprocess.run(['cc','-std=c11','-shared','-fPIC','-O2','-Wall','-Wextra',
                '-o',str(library),str(ROOT/'tests/stress/profile_fsync.c'),'-ldl','-pthread'],check=True)
            source=folder/'subject.c'
            source.write_text(r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <unistd.h>
static void *thread(void *p) {
    int fd=*(int *)p;
    for(int i=0;i<20;i++){errno=EDOM;assert(write(fd,"t",1)==1);assert(errno==EDOM);}
    return 0;
}
int main(int argc,char **argv) {
    assert(argc==3);
    int fd=open(argv[1],O_CREAT|O_RDWR|O_APPEND,0600);assert(fd>=0);
    errno=EDOM;assert(write(fd,"abc",3)==3);assert(errno==EDOM);
    errno=EDOM;assert(pwrite(fd,"x",1,1)==1);assert(errno==EDOM);
    errno=EDOM;assert(write(fd,"",0)==0);assert(errno==EDOM);
    pthread_t ids[4];
    for(int i=0;i<4;i++)assert(pthread_create(&ids[i],0,thread,&fd)==0);
    for(int i=0;i<4;i++)assert(pthread_join(ids[i],0)==0);
    assert(fsync(fd)==0);assert(close(fd)==0);
    fd=open(argv[1],O_RDONLY);assert(fd>=0);
    assert(write(fd,"!",1)==-1 && errno==EBADF);assert(close(fd)==0);
    fd=open(argv[2],O_CREAT|O_WRONLY,0600);assert(fd>=0);
    assert(write(fd,"outside",7)==7);assert(close(fd)==0);
    int wake=eventfd(0,EFD_CLOEXEC);assert(wake>=0);
    uint64_t one=1;
    for(int i=0;i<100;i++)assert(write(wake,&one,sizeof(one))==sizeof(one));
    assert(close(wake)==0);
    fd=open(argv[1],O_WRONLY|O_APPEND);assert(fd==wake);
    assert(write(fd,"r",1)==1);assert(close(fd)==0);
    wake=eventfd(0,EFD_CLOEXEC);assert(wake>=0);
    fd=open(argv[1],O_WRONLY|O_APPEND);assert(fd>=0 && fd!=wake);
    assert(dup2(fd,wake)==wake);assert(close(fd)==0);
    assert(write(wake,"d",1)==1);assert(close(wake)==0);
    wake=eventfd(0,EFD_CLOEXEC);assert(wake>=0);
    assert(syscall(SYS_close,wake)==0); /* Simulate an unobserved library close. */
    fd=openat(AT_FDCWD,argv[1],O_WRONLY|O_APPEND);assert(fd==wake);
    assert(write(fd,"o",1)==1);assert(close(fd)==0);
    return 0;
}
''')
            subject=folder/'subject'
            subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-pthread','-o',str(subject),str(source)],check=True)
            env=dict(os.environ,LD_PRELOAD=str(library),MC_FSYNC_TRACE_PATH=str(folder/'fsync.csv'),
                MC_IO_TRACE_PATH=str(folder/'writes.csv'),MC_IO_TRACE_ROOT=str(world),MC_PROC_FINAL_PATH=str(folder/'final.json'))
            subprocess.run([str(subject),str(world/'file'),str(folder/'world-other')],env=env,check=True,timeout=10)
            with (folder/'writes.csv').open() as trace:
                rows=list(csv.reader(trace))
            final=json.loads((folder/'final.json').read_text())
            self.assertEqual(len(rows),87)
            self.assertEqual(sum(max(0,int(r[5])) for r in rows),87)
            self.assertEqual(sum(int(r[5])<0 for r in rows),1)
            self.assertTrue(all(r[6]==str(world/'file') for r in rows))
            self.assertEqual(final['world_write_calls'],87)
            self.assertEqual(final['world_write_bytes'],87)
            self.assertEqual(final['world_write_errors'],1)
            self.assertEqual(final['write_trace_dropped'],0)


if __name__=='__main__':unittest.main()
