#!/usr/bin/env python3
"""Remote disposable budget/configuration exercise, not a large-RAM capacity claim.
Run in a private systemd unit/cgroup with explicit MemoryMax; main units untouched.
"""
import argparse
import asyncio
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import time
from protocol import varint
from check_player_saves import Instance, barrier, saved
from test_player_visibility import close
from test_player_lifecycle import latest_inventory


def cgroup():
    name=next(line[3:] for line in Path('/proc/self/cgroup').read_text().splitlines() if line.startswith('0::'))
    return Path('/sys/fs/cgroup')/name.lstrip('/')


def metrics(pid):
    status={}
    for line in Path(f'/proc/{pid}/status').read_text().splitlines():
        if line.split(':')[0] in ('VmRSS','VmHWM','VmSize','VmPeak','VmSwap','Threads'):
            key,value=line.split(':',1);status[key]=value.strip()
    status['limits']=next(line for line in Path(f'/proc/{pid}/limits').read_text().splitlines() if line.startswith('Max address space'))
    return status


def group_metrics(group):
    return {name:(group/name).read_text() for name in ('memory.max','memory.high','memory.current','memory.peak','memory.events','memory.pressure','memory.swap.current') if (group/name).exists()}


async def run(args):
    args.binary=args.binary.resolve();args.result_dir=args.result_dir.resolve()
    args.result_dir.mkdir(parents=True,exist_ok=False)
    args.fsync_probe=args.gate_probe=None
    group=cgroup();before=group_metrics(group);filler=None;actors=[];instance=None
    if args.expect_refusal:
        args.memory_budget_mib=args.budgets[0];instance=Instance(args,'refused')
        try:
            await instance.start()
        except AssertionError:
            assert instance.proc.poll()==1
            assert not instance.world.exists()
            assert 'memory budget: cannot determine safe' in (instance.root/'server-1.log').read_text()
        else:
            await instance.stop();raise AssertionError('unsafe budget unexpectedly accepted')
        proof=dict(status='passed',case='insufficient-cgroup-headroom',world_not_created=True,
            before=before,after=group_metrics(group))
        (args.result_dir/'proof.json').write_text(json.dumps(proof,indent=2)+'\n');print(json.dumps(proof));return
    if args.pressure:
        limit=(group/'memory.max').read_text().strip();high=(group/'memory.high').read_text().strip()
        assert limit!='max' and 4*1024**3<=int(limit)<=8*1024**3 and high!='max' and int(high)<int(limit)
        code='import sys,time;data=bytearray(512*1024*1024);data[::4096]=b"x"*len(data[::4096]);print("ready",flush=True);time.sleep(90)'
        filler=subprocess.Popen([sys.executable,'-c',code],stdout=subprocess.PIPE,text=True)
        try:
            assert await asyncio.wait_for(asyncio.to_thread(filler.stdout.readline),40)=='ready\n'
        except BaseException:
            if filler.poll() is None:filler.terminate()
            await asyncio.to_thread(filler.wait)
            raise
    results=[]
    try:
        for budget in args.budgets:
            if filler:assert filler.poll() is None,'pressure helper already exited'
            args.memory_budget_mib=budget
            instance=Instance(args,f'budget-{budget}')
            await instance.start();actors=await instance.clients(1);bot=actors[0]
            await bot.conn.send(56,struct.pack('>h',36)+varint(7)+varint(1)+b'\0\0');await barrier(bot)
            for i in range(1,101):await bot.conn.send(53,struct.pack('>h',i%9))
            await barrier(bot,1)
            assert saved(instance.world,bot.name)==0
            sample=metrics(instance.proc.pid)
            if filler:assert filler.poll() is None,'pressure helper exited during workload'
            stop=time.monotonic();await instance.stop();shutdown=time.monotonic()-stop
            await close(bot);assert saved(instance.world,bot.name)==1
            await instance.start();actors=await instance.clients(1)
            assert actors[0].events[105][-1][1]==b'\1' and latest_inventory(actors[0])[36]==(1,7)
            await close(actors[0]);await instance.stop()
            logs=(instance.root/'server-1.log').read_text()
            assert f'memory budget requested_mib={budget} ' in logs
            results.append(dict(configured_mib=budget,players=1,changes=100,measured=sample,
                shutdown_s=shutdown,restart_inventory=True,budget_log=next(s for s in logs.splitlines() if 'memory budget requested_mib=' in s)))
    finally:
        if filler:
            filler.terminate();await asyncio.to_thread(filler.wait)
        await asyncio.gather(*(close(b) for b in actors if b.conn),return_exceptions=True)
        if instance and instance.proc and instance.proc.poll() is None:await instance.stop()
    after=group_metrics(group)
    def event(data,key):return int(dict(line.split() for line in data['memory.events'].splitlines())[key])
    assert event(after,'oom')==event(before,'oom') and event(after,'oom_kill')==event(before,'oom_kill')
    if args.pressure:assert event(after,'high')>event(before,'high'),'no actual cgroup pressure observed'
    proof=dict(status='passed',server_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
        configured_capacity_not_allocation=True,pressure=args.pressure,pressure_filler_mib=512 if args.pressure else 0,before=before,after=after,profiles=results)
    (args.result_dir/'proof.json').write_text(json.dumps(proof,indent=2)+'\n');print(json.dumps(proof))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary',type=Path,required=True);p.add_argument('--result-dir',type=Path,required=True)
    p.add_argument('--budgets',type=int,nargs='+',default=[4096]);p.add_argument('--pressure',action='store_true')
    p.add_argument('--expect-refusal',action='store_true')
    asyncio.run(run(p.parse_args()))
