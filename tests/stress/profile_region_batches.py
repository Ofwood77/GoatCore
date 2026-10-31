#!/usr/bin/env python3
"""S5 isolated S2 batch study; synthetic NBT, never a mc_server backend.

Reuse S2's existing multi-update transaction API. Atomic and S2 backends receive
the same source reads and immutable NBT batches. No synchronization is removed.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path

from profile_region_txn import invoke, snapshot
from profile_tick import quantile, write_summary


def run(args):
    args.result_dir=args.result_dir.resolve();args.result_dir.mkdir(parents=True,exist_ok=False)
    args.tool=args.tool.resolve();args.fsync_probe=args.fsync_probe.resolve()
    rows=[];references={}
    for count in args.counts:
        for width in args.widths:
            for rep in range(args.repetitions):
                for backend in (['atomic','s2'] if rep%2==0 else ['s2','atomic']):
                    folder=args.result_dir/f'{backend}-{count}-w{width}-r{rep+1}'
                    env=dict(os.environ,LD_PRELOAD=str(args.fsync_probe),
                        MC_SAVE_TRACE_PATH=str(folder/'phases.csv'),MC_FSYNC_TRACE_PATH=str(folder/'fsync.csv'),
                        MC_PROC_FINAL_PATH=str(folder/'process.json'))
                    env.pop('MC_IO_TRACE_PATH',None);env.pop('MC_IO_TRACE_ROOT',None)
                    invoke([args.tool,'--experimental-disposable','bench-batched',folder/'world',backend,count,width,100],folder,env)
                    records=[json.loads(r) for r in (folder/'stdout.log').read_text().splitlines()]
                    start=next(r['us'] for r in records if r.get('phase')=='begin')
                    end=next(r for r in records if r.get('phase')=='end')
                    events=[r for r in records if 'transaction' in r]
                    assert records[-1]['all_chunks_and_opaque_fields_verified']
                    assert end['confirmed']==sum(r['changes'] for r in events)==100
                    assert end['transactions']==len(events)==(100+width-1)//width
                    with (folder/'fsync.csv').open() as file:
                        syncs=[r for r in csv.reader(file) if start<=int(r[0])<end['us']]
                    assert len(syncs)==(7 if backend=='s2' else 2)*len(events)
                    assert all(int(r[4])==0 for r in syncs)
                    disk=snapshot(folder/'world')
                    final_process=json.loads((folder/'process.json').read_text())
                    hashes={k:v['nbt'] for k,v in disk.items()}
                    if count in references:assert references[count]==hashes
                    else:references[count]=hashes
                    row=dict(backend=backend,chunks=count,width=width,repetition=rep+1,
                        confirmed=100,transactions=len(events),wall_ms=end['duration_ns']/1e6,cpu_ms=end['cpu_ns']/1e6,
                        fsync=len(syncs),fsync_wall_ms=sum((int(r[1])-int(r[0]))/1000 for r in syncs),
                        writes=None,
                        transaction_latency_ms={str(p):quantile([r['wall_ns']/1e6 for r in events],p) for p in (.5,.95,.99)},
                        final_process=final_process,
                        region_allocation={k:{a:v[a] for a in ('sectors','free','free_runs','largest_free_run')} for k,v in disk.items()},
                        nbt_all_chunks_equal_to_other_variants=True)
                    rows.append(row);(args.result_dir/'comparison.json').write_text(json.dumps(rows,indent=2))
                    print(json.dumps(row),flush=True)
    # Counting changes the cost in proportion to the number of file writes.
    # Keep it OUTSIDE timed passes, just as the existing ptrace runner does.
    for count in args.counts:
        for width in args.widths:
            for backend in ('atomic','s2'):
                folder=args.result_dir/f'{backend}-{count}-w{width}-counted'
                env=dict(os.environ,LD_PRELOAD=str(args.fsync_probe),
                    MC_IO_TRACE_PATH=str(folder/'writes.csv'),MC_IO_TRACE_ROOT=str(folder/'world'),
                    MC_PROC_FINAL_PATH=str(folder/'process.json'))
                invoke([args.tool,'--experimental-disposable','bench-batched',folder/'world',backend,count,width,100],folder,env)
                records=[json.loads(r) for r in (folder/'stdout.log').read_text().splitlines()]
                start=next(r['us'] for r in records if r.get('phase')=='begin')
                end=next(r for r in records if r.get('phase')=='end')
                assert end['confirmed']==100 and records[-1]['all_chunks_and_opaque_fields_verified']
                disk=snapshot(folder/'world')
                assert references[count]=={k:v['nbt'] for k,v in disk.items()}
                final=json.loads((folder/'process.json').read_text())
                all_writes=write_summary(folder/'writes.csv',0,2**63-1)
                assert final['write_trace_dropped']==0
                assert final['world_write_calls']==all_writes['calls']
                assert final['world_write_bytes']==all_writes['completed_bytes']
                writes=write_summary(folder/'writes.csv',start,end['us'])
                for row in rows:
                    if (row['chunks'],row['width'],row['backend'])==(count,width,backend):
                        row['writes']=writes;row['counted_pass']=str(folder)
    (args.result_dir/'comparison.json').write_text(json.dumps(rows,indent=2))
    (args.result_dir/'manifest.json').write_text(json.dumps(dict(
        tool_sha256=hashlib.sha256(args.tool.read_bytes()).hexdigest(),
        probe_sha256=hashlib.sha256(args.fsync_probe.read_bytes()).hexdigest(),
        passes=len(rows),hardware_note='Local NVMe; synthetic NBT, not playable chunks; no TPS claim or runtime activation'),indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for option in ('tool','fsync-probe','result-dir'):p.add_argument('--'+option,type=Path,required=True)
    p.add_argument('--counts',nargs='+',type=int,choices=(256,1024),default=[256,1024])
    p.add_argument('--widths',nargs='+',type=int,choices=(1,16,64),default=[1,16,64])
    p.add_argument('--repetitions',type=int,default=3)
    args=p.parse_args();assert 1<=args.repetitions<=10;run(args)
