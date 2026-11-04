#!/usr/bin/env python3
"""G1 prototype: disposable-only, independent wire oracle and deterministic faults."""
import argparse
import copy
import ctypes
import hashlib
import json
import os
from pathlib import Path
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from test_world_migration import fixture, write_chunk, snapshot
sys.path.insert(0,str(Path(__file__).resolve().parent/'stress'))
from native_corpus import encode
from native_storage_oracle import chunks, parse, value

TOOL=FAULT=PROBE=None
EVIDENCE=[]
NATIVE_PHASES=('prepare','before-append','record-write','data-partial','before-data-sync',
               'after-data-sync','index-write','index-partial','index-ready',
               'index-file-synced','index-rename-before','index-rename-after',
               'index-published','durable')
SQL_PHASES=('prepare','sql-begin','sql-before-commit','sql-committed','durable',
            'checkpoint-before','checkpoint-after')
COMPACT_NATIVE=('compact-copy','compact-data-partial','compact-data-durable','index-partial',
                'index-file-synced','index-rename-after','compact-published','compact-cleanup','compact-unlinked')


def make_corpus(path):
    path.mkdir()
    for x in (-3,-2,-1):
        root=fixture(x,1)
        root[b'G1Opaque']=(10,{b'blob':(7,bytes(range(256))*256),b'signed':(4,struct.pack('>q',-7754788)),
                            b'nested':(9,(10,[{b'value':(8,b'preserve exact types')}]))})
        (path/f'c_{x}_1.nbt').write_bytes(encode(root))


def roots(region):
    from check_region_storage import reference
    result={}
    for path in region.glob('r.*.*.mca'):
        _,_,records=reference(path)
        for raw in records.values():
            root=parse(raw)
            xy=tuple(int.from_bytes(value(root,k),'big',signed=True) for k in ('xPos','zPos'))
            assert xy not in result
            result[xy]=raw
    return result


def native_oracle(world):
    marker=(world/'GOATWORLD-G1').read_bytes();assert marker[:8]==b'G1WORLD1'
    assert hashlib.sha256(marker[:32]).digest()==marker[32:]
    result={}
    for p in world.glob('*.idx'):
        b=p.read_bytes();assert len(b)==32896 and b[:8]==b'GWIDX001'
        expected=b[96:128];assert hashlib.sha256(b[:96]+bytes(32)+b[128:]).digest()==expected
        version,hlen,dv,dim,rx,rz=struct.unpack_from('>IIIIii',b,8)
        assert (version,hlen,dv,dim)==(1,128,4788,0) and b[64:80]==marker[16:32]
        gen,ident,end=struct.unpack_from('>QQQ',b,32);assert gen>0
        data=(world/f'd.{dim}.r.{rx}.{rz}.data.{ident:016x}').read_bytes()
        assert data[:8]==b'GWLOG001' and data[64:80]==marker[16:32]
        assert hashlib.sha256(data[:96]).digest()==data[96:128] and end<=len(data)
        used=[]
        for slot in range(1024):
            entry=b[128+32*slot:160+32*slot]
            offset,stored,rawlen,generation,codec,layout,level,_=struct.unpack_from('>QIIQBBbB',entry)
            if not offset:assert not any(entry);continue
            assert offset>=4096 and offset+stored<=end and rawlen<=8388608
            used.append((offset,offset+stored));h=data[offset:offset+96]
            assert h[:8]==b'GWREC001' and zlib.crc32(h[:80])==struct.unpack_from('>I',h,80)[0]
            x,z,d=struct.unpack_from('>iiI',h,16);assert (x,z,d)==(rx*32+slot%32,rz*32+slot//32,dim)
            assert struct.unpack_from('>Q',h,40)[0]==generation
            payload=data[offset+96:offset+stored]
            assert codec in (0,1) and layout==0 # golden wire fixture uses zlib6/none
            raw=payload if codec==0 else zlib.decompress(payload)
            assert len(raw)==rawlen and hashlib.sha256(raw).digest()==h[48:80]
            result[(x,z)]=raw
        used.sort();assert all(a[1]<=c[0] for a,c in zip(used,used[1:]))
    return result


class GoatWorld(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='goatcore-g1-cli-');self.base=Path(self.tmp.name)
        self.corpus=self.base/'source';make_corpus(self.corpus);self.before=snapshot(self.corpus)
    def tearDown(self):
        self.assertEqual(snapshot(self.corpus),self.before)
        self.tmp.cleanup()
    def invoke(self,args,expected=0,tool=None,env=None,timeout=60):
        p=subprocess.run([str(tool or TOOL),'--experimental-disposable',*map(str,args)],env=env,
                         capture_output=True,text=True,timeout=timeout)
        self.assertNotRegex(p.stderr,r'AddressSanitizer|LeakSanitizer|ThreadSanitizer|runtime error:')
        self.assertEqual(p.returncode,expected,p.stdout+p.stderr)
        return p
    def seed(self,backend='native',profile='zlib6',name='world'):
        world=self.base/name;self.invoke(['import',self.corpus,world,backend,profile,64]);return world
    def export(self,world,backend,name):
        out=self.base/name;self.invoke(['export',world,backend,out]);return roots(out)
    def revisions(self,world,backend,name):
        result=self.export(world,backend,name)
        return {xy:int.from_bytes(value(parse(raw),'G1BenchRevision',bytes(8)),'big',signed=True) for xy,raw in result.items()}
    def gate(self,world,backend,phase,action=None,compact=False,error=None):
        a,b=os.pipe();c,d=os.pipe()
        env=dict(os.environ,G1_TEST_ROOT=str(world.resolve()),G1_TEST_PHASE=phase,
                 G1_TEST_READY=str(b),G1_TEST_RELEASE=str(c),LD_PRELOAD=(os.environ.get("G1_SANITIZER_PRELOAD", "")+":"+str(PROBE)).lstrip(":"))
        if error:env['G1_TEST_ERROR']=error
        command=['compact',world,backend] if compact else ['mutate',world,backend,self.corpus,'zlib6',64,3,'spread']
        p=subprocess.Popen([str(FAULT),'--experimental-disposable',*map(str,command)],env=env,
                           pass_fds=(b,c),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        os.close(b);os.close(c)
        try:
            ready,_,_=select.select([a],[],[],30)
            self.assertTrue(ready,phase)
            self.assertEqual(os.read(a,1),b'R',(phase,p.poll()))
            if action:action();os.write(d,b'G')
            elif error:os.write(d,b'G')
            else:p.kill()
            out,err=p.communicate(timeout=30)
            self.assertNotRegex(err.decode(),r'AddressSanitizer|LeakSanitizer|ThreadSanitizer|runtime error:')
            return p.returncode,out.decode(),err.decode()
        finally:
            if p.poll() is None:p.kill();p.communicate(timeout=10)
            os.close(a);os.close(d)
    def test_three_backends_and_all_codecs_exact(self):
        profiles=('none','zlib1','zlib6','lz4','zstd1','zstd3','zstd9','lz4-16k','zstd3-16k','none-pack','lz4-pack','zstd3-pack')
        expected={tuple(map(int,p.stem.split('_')[1:])):p.read_bytes() for p in self.corpus.glob('*.nbt')}
        for backend in ('native','sqlite','anvil'):
            for profile in (profiles if backend!='anvil' else ('zlib6',)):
                with self.subTest(backend=backend,profile=profile):
                    world=self.seed(backend,profile,backend+'-'+profile)
                    self.invoke(['verify',world,backend]);self.assertEqual(self.export(world,backend,'export-'+backend+'-'+profile),expected)
                    if backend=='native' and profile in ('none','zlib6'):self.assertEqual(native_oracle(world),expected)
    def test_refs_and_unsafe_inputs_refused(self):
        world=self.seed()
        self.invoke(['import',self.corpus,world,'native','zlib6',64],expected=1)
        self.invoke(['verify',world,'sqlite'],expected=1)
        alias=self.base/'alias';alias.symlink_to(world,target_is_directory=True)
        self.invoke(['verify',alias,'native'],expected=1)
        p=self.corpus/'c_-1_1.nbt';original=p.read_bytes();st=p.stat()
        for status in (b'minecraft:noise',):
            root=parse(original);root[b'Status']=(8,status);p.write_bytes(encode(root))
            self.invoke(['import',self.corpus,self.base/'bad','native','zlib6',64],expected=1)
            self.assertFalse((self.base/'bad').exists())
        p.write_bytes(original);os.utime(p,ns=(st.st_atime_ns,st.st_mtime_ns))
    def test_index_corruption_truncation_and_wrong_world(self):
        original=self.seed();index=next(original.glob('*.idx'));before=index.read_bytes()
        for name,change in [('checksum',lambda b:b.__setitem__(96,b[96]^1)),('version',lambda b:struct.pack_into('>I',b,8,99)),('offset',lambda b:struct.pack_into('>Q',b,128+29*32,1))]:
            world=self.base/name;shutil.copytree(original,world);p=world/index.name;b=bytearray(before);change(b);p.write_bytes(b)
            state=snapshot(world);self.invoke(['verify',world,'native'],expected=1);self.assertEqual(state,snapshot(world))
        other=self.seed(name='different-world');(other/index.name).write_bytes(before)
        self.invoke(['verify',other,'native'],expected=1)
        world=self.base/'lost-index';shutil.copytree(original,world);(world/index.name).unlink();self.invoke(['verify',world,'native'],expected=1)
        world=self.base/'truncated';shutil.copytree(original,world);data=next(world.glob('*.data.*'));data.write_bytes(data.read_bytes()[:-1]);self.invoke(['verify',world,'native'],expected=1)
        world=self.base/'payload';shutil.copytree(original,world);data=next(world.glob('*.data.*'));b=bytearray(data.read_bytes());b[4096+96]^=127;data.write_bytes(b);self.invoke(['verify',world,'native'],expected=1)
    def test_exclusive_writer(self):
        world=self.seed()
        def other():self.invoke(['verify',world,'native'],expected=1)
        code,_,_=self.gate(world,'native','prepare',other)
        self.assertEqual(code,0)
    def test_native_crash_matrix(self):
        original=self.seed()
        for phase in NATIVE_PHASES:
            with self.subTest(phase=phase):
                world=self.base/phase;shutil.copytree(original,world)
                code,_,_=self.gate(world,'native',phase);self.assertEqual(code,-signal.SIGKILL)
                expected=2 if phase in ('index-rename-after','index-published','durable') else 0
                self.assertEqual(set(self.revisions(world,'native','read-'+phase).values()),{expected})
                self.invoke(['verify',world,'native']);self.invoke(['verify',world,'native'])
                EVIDENCE.append(dict(backend='native',phase=phase,revision=expected,idempotent=True))
    def test_sqlite_crash_matrix(self):
        original=self.seed('sqlite')
        for phase in SQL_PHASES:
            with self.subTest(phase=phase):
                world=self.base/phase;shutil.copytree(original,world)
                code,_,_=self.gate(world,'sqlite',phase);self.assertEqual(code,-signal.SIGKILL)
                expected=0 if phase in ('prepare','sql-begin','sql-before-commit') else 2
                self.assertEqual(set(self.revisions(world,'sqlite','read-'+phase).values()),{expected})
                self.invoke(['verify',world,'sqlite']);self.invoke(['verify',world,'sqlite'])
                EVIDENCE.append(dict(backend='sqlite',phase=phase,revision=expected,idempotent=True))
    def test_compaction_crash_matrix(self):
        for backend,phases in [('native',COMPACT_NATIVE),('sqlite',('compact-copy','compact-published','checkpoint-before','checkpoint-after'))]:
            original=self.seed(backend,name='original-'+backend)
            for phase in phases:
                with self.subTest(backend=backend,phase=phase):
                    world=self.base/(backend+'-'+phase);shutil.copytree(original,world)
                    code,_,_=self.gate(world,backend,phase,compact=True);self.assertEqual(code,-signal.SIGKILL)
                    self.assertEqual(set(self.revisions(world,backend,'read-'+backend+'-'+phase).values()),{0})
                    self.invoke(['compact',world,backend]);self.invoke(['verify',world,backend]);self.invoke(['compact',world,backend]);self.invoke(['verify',world,backend])
                    EVIDENCE.append(dict(backend=backend,phase=phase,compaction=True,idempotent=True))
    def test_io_failures_and_retries(self):
        for backend in ('native','sqlite'):
            original=self.seed(backend,name='original-'+backend)
            for i,error in enumerate(('pwrite-EIO','pwrite-ENOSPC','fsync-EIO','fsync-ENOSPC')):
                world=self.base/(backend+str(i));shutil.copytree(original,world)
                code,_,err=self.gate(world,backend,'prepare',error=error)
                self.assertNotEqual(code,0,err)
                revisions=set(self.revisions(world,backend,'read-'+backend+str(i)).values())
                self.assertTrue(revisions in ({0},{2}));self.invoke(['verify',world,backend])
                EVIDENCE.append(dict(backend=backend,error=error,revision=list(revisions),failure_reported=True))
            for i,error in enumerate(('pwrite-short','pwrite-EINTR','fsync-EINTR')):
                world=self.base/(backend+'retry'+str(i));shutil.copytree(original,world)
                code,_,_=self.gate(world,backend,'prepare',error=error)
                revisions=set(self.revisions(world,backend,'retry-read-'+backend+str(i)).values())
                self.assertEqual(revisions,{2} if code==0 else {0});self.invoke(['verify',world,backend])
        original=self.seed(name='uncertain-source');world=self.base/'uncertain';shutil.copytree(original,world)
        code,_,_=self.gate(world,'native','index-rename-after',error='fsync-EIO');self.assertEqual(code,3)
        self.assertEqual(set(self.revisions(world,'native','uncertain-read').values()),{2})
    def test_external_and_unknown_codecs_refused_without_output(self):
        for codec in (4,127,130):
            folder=self.base/('mca-'+str(codec));folder.mkdir()
            data=bytearray(12288);struct.pack_into('>I',data,0,(2<<8)|1)
            struct.pack_into('>I',data,8192,1);data[8196]=codec
            path=folder/'r.0.0.mca';path.write_bytes(data);before=snapshot(folder)
            out=self.base/('unpack-'+str(codec))
            self.invoke(['unpack',folder,out],expected=1)
            self.assertFalse(out.exists());self.assertEqual(snapshot(folder),before)
    def test_repeat_generation_and_sql_identity(self):
        import sqlite3
        for backend in ('native','sqlite','anvil'):
            world=self.seed(backend,name='repeat-'+backend)
            self.invoke(['mutate',world,backend,self.corpus,'zlib6',64,3,'spread'])
            out=self.base/('fresh-'+backend);self.invoke(['export',world,backend,out])
            fresh=self.base/('corpus-'+backend);self.invoke(['unpack',out,fresh])
            self.invoke(['mutate',world,backend,fresh,'zlib6',64,3,'spread'])
            self.assertEqual(set(self.revisions(world,backend,'twice-'+backend).values()),{3})
            self.invoke(['mutate',world,backend,self.corpus,'zlib6',64,3,'spread'],expected=1)
        original=self.seed('sqlite',name='sql-source')
        for change in ('UPDATE chunks SET g=g+1','UPDATE meta SET world_id=zeroblob(16)'):
            world=self.base/('sql-corrupt-'+str(len(change)));shutil.copytree(original,world)
            with sqlite3.connect(world/'chunks.sqlite') as db:db.execute(change)
            self.invoke(['verify',world,'sqlite'],expected=1)
    def test_nested_destination_and_old_version_refused(self):
        self.invoke(['import',self.corpus,self.corpus/'nested','native','zlib6',64],expected=1)
        self.assertFalse((self.corpus/'nested').exists())
        old=self.base/'old';make_corpus(old)
        for p in old.glob('*.nbt'):
            root=parse(p.read_bytes());root[b'DataVersion']=(3,struct.pack('>i',4785));p.write_bytes(encode(root))
        self.invoke(['import',old,self.base/'refused','native','zlib6',64],expected=1)
        self.assertFalse((self.base/'refused').exists())
    def test_short_benchmark_all_work_confirmed(self):
        for backend in ('native','sqlite','anvil'):
            p=self.invoke(['bench',self.corpus,self.base/backend,backend,'zlib6',64,3,128])
            rows=[json.loads(line) for line in p.stdout.splitlines()]
            self.assertEqual(rows[-1]['confirmed_mutations'],6);self.assertTrue(rows[-1]['all_payloads_verified'])
            self.assertEqual(sum(r['updates'] for r in rows if r['phase']=='write'),6)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--tool',required=True,type=Path);p.add_argument('--fault-tool',required=True,type=Path);p.add_argument('--fault-probe',type=Path);p.add_argument('--evidence',type=Path)
    args=p.parse_args();TOOL=args.tool.resolve();FAULT=args.fault_tool.resolve()
    if args.fault_probe:PROBE=args.fault_probe.resolve()
    else:
        folder=tempfile.TemporaryDirectory(prefix='goatcore-g1-fault-');PROBE=Path(folder.name)/'fault.so'
        subprocess.run(['cc','-std=c11','-shared','-fPIC','-O2','-Wall','-Wextra','-o',str(PROBE),str(Path(__file__).parent/'stress/goatworld_fault.c')],check=True)
    result=unittest.main(argv=[sys.argv[0]],exit=False)
    if args.evidence:args.evidence.write_text(json.dumps(dict(status='passed' if result.result.wasSuccessful() else 'failed',tests=result.result.testsRun,events=EVIDENCE),indent=2)+'\n')
    sys.exit(0 if result.result.wasSuccessful() else 1)
