#!/usr/bin/env python3
"""Disposable G1 backend comparison. No Minecraft server is launched here."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import shutil
import statistics
import subprocess
import time


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def percentile(values,p):
    values=sorted(values)
    return values[min(len(values)-1,max(0,__import__('math').ceil(len(values)*p)-1))] if values else None

def limits():
    resource.setrlimit(resource.RLIMIT_AS,(1024**3,1024**3))
    resource.setrlimit(resource.RLIMIT_FSIZE,(1024**3,1024**3))

def summarize(rows):
    result={r['phase']:r for r in rows if r['phase'] not in ('read','write','allocation')}
    for phase in ('read','write'):
        for pattern in sorted({r['pattern'] for r in rows if r['phase']==phase}):
            samples=[r['wall_ns'] for r in rows if r['phase']==phase and r['pattern']==pattern]
            result[phase+'-'+pattern]=dict(samples=len(samples),p50_ns=percentile(samples,.5),p95_ns=percentile(samples,.95),p99_ns=percentile(samples,.99),max_ns=max(samples))
    result['allocation']={r['stage']:r for r in rows if r['phase']=='allocation'}
    assert result['complete']['all_payloads_verified']
    assert sum(r['updates'] for r in rows if r['phase']=='write')==result['complete']['confirmed_mutations']
    return result


def run(args):
    tool=args.tool.resolve();corpora=args.corpora.resolve();out=args.result_dir.resolve()
    out.mkdir(parents=True,exist_ok=False);lab=out/'disposable';lab.mkdir()
    before={str(p.relative_to(corpora)):sha(p) for p in corpora.rglob('*') if p.is_file()}
    manifest=dict(hardware_label=args.hardware_label,tool_sha256=sha(tool),versions=subprocess.check_output([tool,'--experimental-disposable','versions'],text=True).strip(),
        platform=platform.platform(),cpu=Path('/proc/cpuinfo').read_text().split('model name')[1].splitlines()[0] if Path('/proc/cpuinfo').exists() else '',
        command=__import__('sys').argv,repetitions=args.repetitions,source_files=before,cache_policy='per-file fadvise DONTNEED then mincore; no device cache eviction',
        guarantee='same raw NBT; zlib6 shared comparison; checked durable close/checkpoint and compact included; not mc_server TPS')
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    records=[];signatures={}
    cases=[]
    for corpus in filter(None,args.cases.split(',')):
        for rep in range(args.repetitions):
            order=['anvil','native','sqlite'];order=order[rep%3:]+order[:rep%3]
            cases.extend((corpus,b,'zlib6',rep,False) for b in order)
    if args.profiles:
        for profile in ('none','lz4','zstd1','zstd3','zstd9','lz4-16k','zstd3-16k','none-pack','lz4-pack','zstd3-pack'):
            for rep in range(args.repetitions):
                cases.extend(('mixed32',b,profile,rep,False) for b in ('native','sqlite'))
    if args.probe:
        for corpus in ('483','1000','spread128'):
            for b in ('anvil','native','sqlite'):cases.append((corpus,b,'zlib6',0,True))
    for corpus,b,profile,rep,counted in cases:
        tag=f'{corpus}-{b}-{profile}-{rep}'+('-counted' if counted else '')
        root=lab/tag;env=os.environ.copy();env.pop('LD_PRELOAD',None)
        if counted:
            env.update(LD_PRELOAD=str(args.probe.resolve()),MC_FSYNC_TRACE_PATH=str(out/(tag+'.sync.csv')),MC_IO_TRACE_PATH=str(out/(tag+'.write.csv')),MC_IO_TRACE_ROOT=str(root),MC_PROC_FINAL_PATH=str(out/(tag+'.proc.json')))
        cmd=[str(tool),'--experimental-disposable','bench',str(corpora/corpus),str(root),b,profile,'64',str(args.changes),str(args.reads)]
        start=time.monotonic()
        with (out/(tag+'.jsonl')).open('w') as stdout,(out/(tag+'.stderr')).open('w') as stderr:
            p=subprocess.run(cmd,stdout=stdout,stderr=stderr,env=env,timeout=args.timeout,preexec_fn=limits)
        if p.returncode:raise RuntimeError((tag,p.returncode,(out/(tag+'.stderr')).read_text()))
        rows=[json.loads(line) for line in (out/(tag+'.jsonl')).read_text().splitlines()]
        summary=summarize(rows);assert summary['complete']['confirmed_mutations']==2*args.changes
        key=corpus;signature=summary['complete']['payload_sha256'];assert signatures.setdefault(key,signature)==signature
        record=dict(corpus=corpus,backend=b,profile=profile,repetition=rep,counted=counted,process_wall_s=time.monotonic()-start,summary=summary)
        if counted:
            proc=json.loads((out/(tag+'.proc.json')).read_text());record['process']=proc
            with (out/(tag+'.write.csv')).open() as file:writes=list(csv.reader(file))
            with (out/(tag+'.sync.csv')).open() as file:syncs=list(csv.reader(file))
            assert len(writes)==proc['world_write_calls'] and sum(max(0,int(r[5])) for r in writes)==proc['world_write_bytes'] and proc['write_trace_dropped']==0
            assert len(syncs)==proc['fsync_calls']+proc['fdatasync_calls'] and all(int(r[4])==0 for r in syncs)
            record['sync_wall_ns']=sum((int(r[1])-int(r[0]))*1000 for r in syncs)
        records.append(record)
        with (out/'summary.jsonl').open('a') as file:file.write(json.dumps(record)+'\n')
        shutil.rmtree(root) # only this runner's freshly created, verified disposable output
        print(tag,round(record['process_wall_s'],3),flush=True)
    if args.codec:
        for corpus in ('483','official-full','1000','mixed32','uniform','diverse','opaque'):
            for rep in range(args.repetitions):
                with (out/f'codec-{corpus}-{rep}.jsonl').open('w') as file:
                    subprocess.run([tool,'--experimental-disposable','codec',corpora/corpus,'1'],stdout=file,check=True,timeout=args.timeout,preexec_fn=limits)
    assert before=={str(p.relative_to(corpora)):sha(p) for p in corpora.rglob('*') if p.is_file()}
    (out/'complete.json').write_text(json.dumps(dict(status='passed',runs=len(records),all_sources_unchanged=True,all_work_equal=True,signatures=signatures),indent=2)+'\n')
    lab.rmdir()

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--tool',required=True,type=Path);p.add_argument('--corpora',required=True,type=Path);p.add_argument('--result-dir',required=True,type=Path);p.add_argument('--hardware-label',required=True);p.add_argument('--repetitions',type=int,default=3);p.add_argument('--cases',default='1,100,483,1000,spread128');p.add_argument('--profiles',action='store_true');p.add_argument('--codec',action='store_true');p.add_argument('--probe',type=Path);p.add_argument('--changes',type=int,default=100);p.add_argument('--reads',type=int,default=1000);p.add_argument('--timeout',type=int,default=600)
    run(p.parse_args())
