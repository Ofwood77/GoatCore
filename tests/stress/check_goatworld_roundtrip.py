#!/usr/bin/env python3
"""Remote-only G1 chunk archive/export, GoatCore and official Vanilla validation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import select
import struct
import socket
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests'))
from test_world_migration import snapshot
from native_storage_oracle import chunks,same_blocks
from native_corpus import compare_entities,encode
from native_storage_oracle import parse


def port():
    with socket.socket() as s:s.bind(('127.0.0.1',0));n=s.getsockname()[1]
    assert n not in (25565,25587);return n


def run(a):
    source=a.source.resolve();before=snapshot(source);out=a.result_dir.resolve();out.mkdir(parents=True,exist_ok=False)
    tool=a.tool.resolve();server=a.server.resolve()
    def execute(tag,cmd):
        with (out/(tag+'.log')).open('w') as file:subprocess.run(list(map(str,cmd)),stdout=file,stderr=subprocess.STDOUT,check=True,timeout=900,cwd=ROOT)
    corpus=out/'corpus';archive=out/'archive';regions=out/'export'
    prefix=[tool,'--experimental-disposable']
    execute('unpack',prefix+['unpack',source/'dimensions/minecraft/overworld/region',corpus])
    execute('import',prefix+['import',corpus,archive,a.backend,'zstd3-pack',64])
    if a.crash_after_durable:
        assert a.fault_tool and a.fault_probe and len(list(corpus.glob('*.nbt')))==1
        ready,notify=os.pipe();release,go=os.pipe()
        env=dict(os.environ,G1_TEST_ROOT=str(archive),G1_TEST_PHASE='durable',G1_TEST_READY=str(notify),G1_TEST_RELEASE=str(release),LD_PRELOAD=str(a.fault_probe.resolve()))
        with (out/'crash.stdout').open('w') as stdout,(out/'crash.stderr').open('w') as stderr:
            process=subprocess.Popen([str(a.fault_tool.resolve()),'--experimental-disposable','mutate',str(archive),a.backend,str(corpus),'zstd3-pack','64','1','same'],env=env,pass_fds=(notify,release),stdout=stdout,stderr=stderr)
            os.close(notify);os.close(release)
            try:
                assert select.select([ready],[],[],30)[0] and os.read(ready,1)==b'R'
                process.kill();assert process.wait(timeout=10)==-9
            finally:
                if process.poll() is None:process.kill();process.wait(timeout=10)
                os.close(ready);os.close(go)
    execute('verify',prefix+['verify',archive,a.backend])
    execute('export',prefix+['export',archive,a.backend,regions])
    expected={xy:raw for xy,root,raw in chunks(source)}
    if a.crash_after_durable:
        for xy,raw in list(expected.items()):
            root=parse(raw);root[b'G1BenchRevision']=(4,struct.pack('>q',2));expected[xy]=encode(root)
    world=out/'world-copy';shutil.copytree(source,world)
    rd=world/'dimensions/minecraft/overworld/region'
    for path in rd.glob('*.mca'):path.unlink() # only this fresh disposable copy
    for path in regions.glob('*.mca'):shutil.copy2(path,rd/path.name)
    actual={xy:raw for xy,root,raw in chunks(world)};assert expected==actual
    added=[]
    for name in ('level.dat','level.dat_old','data','datapacks'):
        src=a.metadata_template/name;dst=world/name
        if src.exists() and not dst.exists():
            if src.is_dir():shutil.copytree(src,dst)
            else:shutil.copy2(src,dst)
            added.append(name)
    assert (world/'level.dat').exists()
    runner=[sys.executable,ROOT/'tests/stress/run_block_checks.py']
    common=['--suite',a.suite,'--verify-only','--snapshot-template',a.snapshot.resolve(),'--verify-restarts','2','--view-distance','2','--shutdown-timeout','180']
    execute('gc',runner+['--kind','goatcore','--server-binary',server,'--chunk-storage','anvil-atomic-experimental','--anvil-save-mode','async','--port',port(),'--world-template',world,'--result-dir',out/'gc',*common])
    gc=same_blocks(world,out/'gc/runtime/world');assert compare_entities(world,out/'gc/runtime/world')['status']=='passed'
    execute('vanilla',runner+['--kind','vanilla','--vanilla-root',a.vanilla_root.resolve(),'--java',a.java.resolve(),'--port',port(),'--world-template',world,'--result-dir',out/'vanilla',*common])
    official=same_blocks(world,out/'vanilla/runtime/world');entities=compare_entities(world,out/'vanilla/runtime/world')
    assert before==snapshot(source)
    proof=dict(status='passed',crash_after_durable=a.crash_after_durable,backend=a.backend,suite=a.suite,chunks=len(expected),byte_exact_export=True,goatcore_blocks=gc,vanilla_blocks=official,entities=entities,source_unchanged=True,
        added_metadata=added,composition='disposable chunks-only roundtrip; global files copied independently, never claimed GoatWorld encoded',tool_sha256=hashlib.sha256(tool.read_bytes()).hexdigest(),server_sha256=hashlib.sha256(server.read_bytes()).hexdigest())
    (out/'proof.json').write_text(json.dumps(proof,indent=2)+'\n');print(json.dumps({k:v for k,v in proof.items() if k not in ('goatcore_blocks','vanilla_blocks')}))

if __name__=='__main__':
    p=argparse.ArgumentParser()
    for name in ('source','tool','server','snapshot','metadata-template','result-dir','vanilla-root','java'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--crash-after-durable',action='store_true');p.add_argument('--fault-tool',type=Path);p.add_argument('--fault-probe',type=Path)
    p.add_argument('--backend',choices=('native','sqlite'),required=True);p.add_argument('--suite',choices=('stairs','complex'),required=True)
    run(p.parse_args())
