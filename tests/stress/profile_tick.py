#!/usr/bin/env python3
"""Remote-only continuous tick/IO comparison using existing protocol-775 bots.

Never run on a developer PC. Binaries must contain the same optional tick trace.
Every run copies one immutable world; startup, preparation, warmup and idle are
separate from the fixed action phase. All bots stay connected throughout.
"""
import argparse
import asyncio
import csv
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import signal
import socket
import statistics
import struct
import subprocess
import time

from protocol import position, varint, read_varint
from stress import Bot, block_target_offset, options
from test_player_visibility import close

ROOT = Path(__file__).resolve().parents[2]


class ProfileBot(Bot):
    def __init__(self, *args):
        super().__init__(*args)
        self.pickups=[]

    async def play_packet(self, packet_id, payload):
        if packet_id==124:  # TakeItemEntity, protocol 775
            _,offset=read_varint(payload)
            _,offset=read_varint(payload,offset)
            count,end=read_varint(payload,offset)
            assert end==len(payload)
            self.pickups.append((time.monotonic_ns()//1000,count))
        await super().play_packet(packet_id,payload)


def quantile(values, p):
    """Nearest rank, with no histogram clipping."""
    ordered = sorted(values)
    return ordered[max(0, math.ceil(p * len(ordered)) - 1)] if ordered else None


def summarize(rows, start, end):
    # Costs of all ticks overlapping the phase; rate counts actual tick starts.
    # Include the final crossing tick, never drop a slow tail or incomplete window.
    selected = [r for r in rows if r['start_us'] < end and r['end_us'] > start]
    started = [r for r in rows if start <= r['start_us'] < end]
    costs = [(r['end_us'] - r['start_us']) / 1000 for r in selected]
    busy = [(r['end_us'] - r['start_us'] + r['fast_us']) / 1000 for r in selected]
    intervals = [(b['start_us']-a['start_us'])/1000 for a,b in zip(started,started[1:])]
    duration = (end - start) / 1e6
    phases = ('tasks_us', 'fast_us', 'world_us', 'proto_us', 'items_us', 'remote_us',
              'updates_us', 'evict_us', 'handlers_us', 'flush_us', 'world_save_us',
              'integrate_us', 'items_lock_us', 'furnace_lock_us')
    return dict(duration_s=duration, ticks_started=len(started), ticks_overlapping=len(selected),
                tps=len(started)/duration, p50_ms=quantile(costs,.5), p95_ms=quantile(costs,.95),
                p99_ms=quantile(costs,.99), max_ms=max(costs,default=None),
                over_50ms=sum(v>50 for v in costs),
                boundary_ticks=sum(r['start_us']<start or r['end_us']>end for r in selected),
                tick_and_fast_ms={str(p):quantile(busy,p) for p in (.5,.95,.99)},
                interarrival_ms={str(p):quantile(intervals,p) for p in (.5,.95,.99)},
                scheduled_tick_wall_s=sum(costs)/1000,
                scheduled_tick_cpu_s=sum(r['cpu_us'] for r in selected)/1e6,
                tick_thread_interval_cpu_s=sum(r['interval_cpu_us'] for r in selected)/1e6,
                costs_s={k:sum(r[k] for r in selected)/1e6 for k in phases},
                task_queue_depth_max=max((r['task_depth'] for r in selected),default=0),
                task_queue_peak=max((r['task_peak'] for r in selected),default=0),
                task_wait_max_ms=max((r['task_wait_max_us'] for r in selected),default=0)/1000,
                item_entities_max=max((r['items'] for r in selected),default=0),
                ready_min=min((r['ready'] for r in selected),default=0),
                ready_max=max((r['ready'] for r in selected),default=0))


def read_trace(path):
    with path.open() as f:
        rows = [{k:int(v) for k,v in row.items()} for row in csv.DictReader(f)]
    assert rows, 'empty trace'
    for i,row in enumerate(rows):
        assert row['idx']==i and row['end_us']>=row['start_us'], 'incomplete/out-of-order tick trace'
        if i: assert row['start_us']>=rows[i-1]['end_us'], 'overlapping ticks'
    return rows


def io_summary(path, start, end):
    calls=[]
    if path.exists():
        with path.open() as f:
            for row in csv.reader(f):
                a,b,cpu,tid,rc=map(int,row[:5])
                if a<end and b>start:calls.append((a,b,cpu,tid,rc,row[5]))
    return dict(calls=len(calls), wall_s=sum(b-a for a,b,*_ in calls)/1e6,
                cpu_s=sum(c for a,b,c,*_ in calls)/1e6,
                max_ms=max(((b-a)/1000 for a,b,*_ in calls),default=0),
                errors=sum(c[4]!=0 for c in calls))


def proc_sample(pid):
    base=Path(f'/proc/{pid}')
    fields=(base/'stat').read_text().rsplit(')',1)[1].split()
    status=dict(line.split(':',1) for line in (base/'status').read_text().splitlines() if ':' in line)
    return dict(time_us=time.monotonic_ns()//1000,
                cpu_s=(int(fields[11])+int(fields[12]))/os.sysconf('SC_CLK_TCK'),
                rss_kb=int(status['VmRSS'].split()[0]), hwm_kb=int(status['VmHWM'].split()[0]),
                process_io=(base/'io').read_text(), host_stat=Path('/proc/stat').read_text().splitlines()[0],
                loadavg=Path('/proc/loadavg').read_text().strip(),
                pressure={k:Path(f'/proc/pressure/{k}').read_text() for k in ('cpu','io','memory')},
                threads={p.name:dict(schedstat=(p/'schedstat').read_text().strip(),wchan=(p/'wchan').read_text().strip())
                         for p in (base/'task').iterdir()})


async def monitor(pid, samples, stop):
    while not stop.is_set():
        try:
            sample=proc_sample(pid)
            client=Path('/proc/self/stat').read_text().rsplit(')',1)[1].split()
            sample['client_cpu_s']=(int(client[11])+int(client[12]))/os.sysconf('SC_CLK_TCK')
            sample['client_rss_kb']=int(Path('/proc/self/status').read_text().split('VmRSS:')[1].split()[0])
            samples.append(sample)
        except (FileNotFoundError,ProcessLookupError):break
        await asyncio.sleep(.25)


async def exercise(port, bots, cycles, item, result, warmup, idle, prepare_only):
    opts=options(['--host','127.0.0.1','--port',str(port),'--mode','blocks','--bots',str(bots),
                  '--allow-block-changes','--interval','0.001','--block-cooldown','0.01',
                  '--timeout','60','--block-item-id',str(item)])
    opts.run_id='PROF';opts.place_limiter=None;opts.break_limiter=None
    bot_log=(result/'bots.log').open('w')
    def log(message):
        bot_log.write(datetime.now(timezone.utc).isoformat()+' '+message+'\n');bot_log.flush()
    actors=[ProfileBot(i,opts,log) for i in range(bots)]
    phases={}
    now=lambda:time.monotonic_ns()//1000
    async def connect(bot):
        await bot.connect()
        # No more chunk streaming work enters the measured phase.
        deadline=time.monotonic()+120
        while sum(e.is_set() for e in bot.chunk_ready_events.values())<441:
            if time.monotonic()>deadline:raise TimeoutError('all 441 view-distance 10 chunks')
            await asyncio.sleep(.05)
        await bot.setup_block_item()
        ox,oz=block_target_offset(bot.index)
        point=(math.floor(bot.pos[0])+ox,math.floor(bot.pos[1]),math.floor(bot.pos[2])+oz)
        bot.clear_block_events(point);bot.sequence+=1
        await bot.conn.send(41,varint(0)+position(*point)+varint(1)+varint(bot.sequence))
        assert await bot.expect_block(point,lambda s:s==0) is not None, 'prepare target'
        bot.block_prepared=True
        bot.profile_target=point
    try:
        start=now()
        # Bounded spawn staggering, same deterministic identities for every copy.
        jobs=[]
        for bot in actors:
            jobs.append(asyncio.create_task(connect(bot)));await asyncio.sleep(.02)
        await asyncio.gather(*jobs)
        phases['prepare']=[start,now()]
        start=now();await asyncio.sleep(warmup);phases['warmup']=[start,now()]
        start=now();await asyncio.sleep(idle);phases['idle_before']=[start,now()]
        for bot in actors:
            bot.latencies.clear();bot.failures=0
        async def work(bot):
            for _ in range(cycles):
                if not await bot.block_cycle(bot.profile_target):break
                bot.cycles_completed+=1
                await asyncio.sleep(.001)
        start=now()
        if not prepare_only:await asyncio.wait_for(asyncio.gather(*(work(b) for b in actors)),300)
        else:await asyncio.sleep(.001)
        phases['active']=[start,now()]
        start=now();await asyncio.sleep(idle);phases['idle_after']=[start,now()]
        assert all(b.state=='play' and not b.error for b in actors), 'connection failure'
        latencies=[v for b in actors for v in b.latencies]
        report=dict(phases=phases,bots=[b.report() for b in actors],
                    action_times=[dict(place_sent=b.place_send_times,break_sent=b.break_send_times,
                                       place_confirmed=b.place_verified_times,break_confirmed=b.break_verified_times,
                                       latencies_ms=b.latencies) for b in actors],
                    pickup_observations=[dict(events=len(b.pickups),items=sum(n for _,n in b.pickups)) for b in actors],
                    pickup_events_first_observer=actors[0].pickups,
                    placed=sum(b.placed for b in actors),broken=sum(b.broken for b in actors),
                    missing=sum(cycles-b.cycles_completed for b in actors) if not prepare_only else 0,
                    failures=sum(b.failures for b in actors),
                    latencies_ms=dict(mean=statistics.mean(latencies) if latencies else None,
                                      p50=quantile(latencies,.5),p95=quantile(latencies,.95),p99=quantile(latencies,.99),
                                      maximum=max(latencies,default=None)))
        (result/'actions.json').write_text(json.dumps(report,indent=2))
        assert report['missing']==report['failures']==0, 'unconfirmed actions'
        return report
    finally:
        await asyncio.gather(*(close(b) for b in actors if b.conn),return_exceptions=True)
        bot_log.close()


async def one(args,binary,label,bots,result,template=None,prepare_only=False):
    result.mkdir(parents=True,exist_ok=False)
    runtime=result/'runtime';runtime.mkdir();world=runtime/'world'
    if template:shutil.copytree(template,world)
    (runtime/'assets').symlink_to(ROOT/'assets')
    (runtime/'server.config').write_text('online-mode=false\noffline-skins=false\nview-distance=10\nsimulation-distance=10\nlevel-seed=0\n')
    with socket.socket() as sock:
        sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    assert port not in (25565,25587)
    env=dict(os.environ,MC_WORLD_PATH=str(world),MC_BIND_PORT=str(port),
             MC_SETUP_FILE=str(ROOT/'tests/stress/network/one.conf'),MC_PERF='1',MC_PERF_SLOW_MS='50',
             MC_PERF_SUMMARY_TICKS='100',MC_PERF_TRACE_PATH=str(result/'ticks.csv'))
    if args.fsync_probe:
        env.update(LD_PRELOAD=str(args.fsync_probe.resolve()),MC_FSYNC_TRACE_PATH=str(result/'fsync.csv'))
    if args.save_trace:env['MC_SAVE_TRACE_PATH']=str(result/'saves.csv')
    samples=[];stop=asyncio.Event()
    (result/'processes-before.txt').write_text(subprocess.check_output(['ps','-eo','pid,comm,pcpu,pmem,args','--sort=-pcpu'],text=True))
    started=time.monotonic()
    with (result/'server.log').open('w') as log:
        proc=subprocess.Popen([str(binary)],cwd=runtime,env=env,stdout=log,stderr=subprocess.STDOUT)
        sampling=asyncio.create_task(monitor(proc.pid,samples,stop))
        try:
            deadline=time.monotonic()+30
            while True:
                if proc.poll() is not None:raise RuntimeError(f'startup exit {proc.returncode}')
                try:
                    with socket.create_connection(('127.0.0.1',port),timeout=.2):break
                except ConnectionRefusedError:
                    if time.monotonic()>deadline:raise TimeoutError('listen')
                    await asyncio.sleep(.05)
            report=await exercise(port,bots,args.cycles,args.item,result,args.warmup,args.idle,prepare_only)
        finally:
            stop.set();await sampling
            (result/'resources.json').write_text(json.dumps(samples,indent=2))
            if proc.poll() is None:proc.send_signal(signal.SIGTERM)
            try:await asyncio.to_thread(proc.wait,60)
            except subprocess.TimeoutExpired:
                proc.kill();await asyncio.to_thread(proc.wait);raise RuntimeError('unclean shutdown')
        assert proc.returncode==0, f'server exit {proc.returncode}'
    rows=read_trace(result/'ticks.csv')
    if args.fsync_probe:assert (result/'fsync.csv').is_file(), 'fsync probe did not produce a trace'
    assert rows[0]['start_us'] <= report['phases']['active'][0] and rows[-1]['end_us'] >= report['phases']['active'][1], 'active phase not fully traced'
    summaries={}
    for phase,(a,b) in report['phases'].items():
        if b<=a:continue
        stats=summarize(rows,a,b);stats['fsync']=io_summary(result/'fsync.csv',a,b) if args.fsync_probe else None
        selected=[s for s in samples if a<=s['time_us']<=b]
        if len(selected)>1:
            stats['process_cpu_percent']=100*(selected[-1]['cpu_s']-selected[0]['cpu_s'])/((selected[-1]['time_us']-selected[0]['time_us'])/1e6)
            stats['client_cpu_percent']=100*(selected[-1]['client_cpu_s']-selected[0]['client_cpu_s'])/((selected[-1]['time_us']-selected[0]['time_us'])/1e6)
            stats['rss_min_kb']=min(s['rss_kb'] for s in selected);stats['rss_max_kb']=max(s['rss_kb'] for s in selected)
        summaries[phase]=stats
    result_row=dict(label=label,binary=str(binary),sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),bots=bots,
                    cycles_per_bot=args.cycles,item_id=args.item,port=port,phases=summaries,
                    placed=report['placed'],broken=report['broken'],missing=report['missing'],failures=report['failures'],
                    pickup_observations=report['pickup_observations'],
                    latencies_ms=report['latencies_ms'],total_including_shutdown_s=time.monotonic()-started)
    (result/'summary.json').write_text(json.dumps(result_row,indent=2))
    print(json.dumps(dict(label=label,bots=bots,result=str(result),placed=report['placed'],broken=report['broken'],active=summaries['active'])),flush=True)
    return result_row


async def main(args):
    args.result_dir=args.result_dir.resolve();args.result_dir.mkdir(parents=True,exist_ok=False)
    binaries=[]
    for value in args.binary:
        label,path=value.split('=',1);binary=Path(path).resolve();assert binary.is_file()
        assert label and all(c.isalnum() or c in '-_' for c in label)
        binaries.append((label,binary))
    if args.world_template:template=args.world_template.resolve()
    else:
        await one(args,binaries[0][1],'template',1,args.result_dir/'template',prepare_only=True)
        template=args.result_dir/'template/runtime/world'
    manifest={str(p.relative_to(template)):hashlib.sha256(p.read_bytes()).hexdigest() for p in template.rglob('*') if p.is_file()}
    (args.result_dir/'template-manifest.json').write_text(json.dumps(manifest,indent=2))
    rows=[]
    for bots in args.bots:
        for repetition in range(args.repetitions):
            order=binaries if repetition%2==0 else list(reversed(binaries))
            for label,binary in order:
                name=f'{label}-{bots}-r{repetition+1}'
                row=await one(args,binary,label,bots,args.result_dir/name,template)
                row['repetition']=repetition+1;rows.append(row)
                (args.result_dir/'comparison.json').write_text(json.dumps(rows,indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary',action='append',required=True,help='label=/absolute/path/to/binary')
    p.add_argument('--bots',nargs='+',type=int,default=[1,10,25,50])
    p.add_argument('--repetitions',type=int,default=3)
    p.add_argument('--cycles',type=int,default=100)
    p.add_argument('--item',type=int,default=1)
    p.add_argument('--warmup',type=float,default=5)
    p.add_argument('--idle',type=float,default=5)
    p.add_argument('--fsync-probe',type=Path)
    p.add_argument('--save-trace',action='store_true',help='record storage stages and server action queue/handler times')
    p.add_argument('--world-template',type=Path)
    p.add_argument('--result-dir',type=Path,required=True)
    args=p.parse_args()
    if any(not 1<=n<=50 for n in args.bots) or args.cycles<1 or args.repetitions<1:p.error('positive cycles/repetitions and 1..50 bots required')
    if not all(math.isfinite(x) and x>=0 for x in (args.warmup,args.idle)):p.error('invalid phase duration')
    if args.fsync_probe and not args.fsync_probe.is_file():p.error('fsync probe library not found')
    asyncio.run(main(args))
