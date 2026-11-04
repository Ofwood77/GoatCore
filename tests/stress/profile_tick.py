#!/usr/bin/env python3
"""Continuous tick/IO comparison using existing protocol-775 bots.

Use the dedicated host, or an explicitly authorized isolated local exception. Binaries must contain the same optional tick trace.
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
from native_storage_oracle import world_blocks, chunks, sections, section_blocks
from test_player_visibility import close
from test_block_persistence import query_block
from test_block_placement import report_states

ROOT = Path(__file__).resolve().parents[2]


class ProfileBot(Bot):
    def __init__(self, *args):
        super().__init__(*args)
        self.pickups=[]
        self.ack_sequence=-1
        self.ack_event=asyncio.Event()
        self.acknowledged=[]

    async def play_packet(self, packet_id, payload):
        if packet_id==4:  # BlockChangedAck, protocol 775; not a disk acknowledgement
            sequence,end=read_varint(payload)
            assert end==len(payload), "malformed block ACK"
            self.ack_sequence=max(self.ack_sequence,sequence)
            self.ack_event.set()
        if packet_id==124:  # TakeItemEntity, protocol 775
            _,offset=read_varint(payload)
            _,offset=read_varint(payload,offset)
            count,end=read_varint(payload,offset)
            assert end==len(payload)
            self.pickups.append((time.monotonic_ns()//1000,count))
        await super().play_packet(packet_id,payload)

    async def expect_block(self, block, predicate):
        # A state packet alone may be an earlier broadcast. Also wait until the
        # server has processed THIS sequence before sending the next action.
        # Combined with a private target and alternating known states this
        # prevents a delayed display update from counting as completed work.
        sequence=self.sequence;started=time.monotonic()
        state=await super().expect_block(block,predicate)
        if state is None:return None
        try:
            while self.ack_sequence<sequence:
                self.ack_event.clear()
                await asyncio.wait_for(self.ack_event.wait(),self.args.timeout)
        except TimeoutError:return None
        self.latencies[-1]=(time.monotonic()-started)*1000
        self.acknowledged.append(dict(sequence=sequence,state=state,time_us=time.monotonic_ns()//1000))
        return state


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
    # Fast passes occur BETWEEN scheduled ticks. A slow final pass can end in
    # the next row AFTER phase end. Keep its whole aggregate instead of silently
    # dropping that tail. First/last fast intervals may include adjacent work;
    # expose these boundary intervals rather than claim exact time slicing.
    busy=[];fast_covered=0;busy_boundaries=0
    for index,row in enumerate(rows):
        previous_end=rows[index-1]['end_us'] if index else row['start_us']-row['fast_us']
        fixed=row['start_us']<end and row['end_us']>start
        fast=row['fast_us']>0 and previous_end<end and row['start_us']>start
        if not fixed and not fast:continue
        busy.append(((row['end_us']-row['start_us']) if fixed else 0)/1000+(row['fast_us']/1000 if fast else 0))
        if fast:
            fast_covered+=row['fast_us']
            busy_boundaries+=previous_end<start or row['start_us']>end
    intervals = [(b['start_us']-a['start_us'])/1000 for a,b in zip(started,started[1:])]
    duration = (end - start) / 1e6
    phases = ('tasks_us', 'fast_us', 'world_us', 'proto_us', 'items_us', 'remote_us',
              'updates_us', 'evict_us', 'handlers_us', 'flush_us', 'world_save_us',
              'integrate_us', 'items_lock_us', 'furnace_lock_us')
    return dict(duration_s=duration, ticks_started=len(started), ticks_overlapping=len(selected),
                tps=len(started)/duration, p50_ms=quantile(costs,.5), p95_ms=quantile(costs,.95),
                p99_ms=quantile(costs,.99), max_ms=max(costs,default=None),
                over_50ms=sum(v>50 for v in costs),
                busy_max_ms=max(busy,default=None),busy_over_50ms=sum(v>50 for v in busy),
                busy_samples=len(busy),busy_boundary_intervals=busy_boundaries,covered_fast_s=fast_covered/1e6,
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


def write_summary(path, start, end):
    calls=[]
    with path.open() as file:
        for row in csv.reader(file):
            a,b,tid=int(row[0]),int(row[1]),int(row[2])
            if a<end and b>start:
                calls.append((row[3],int(row[4]),int(row[5]),row[6]))
    return dict(calls=len(calls),requested_bytes=sum(r[1] for r in calls),
                completed_bytes=sum(max(0,r[2]) for r in calls),
                errors=sum(r[2]<0 for r in calls),
                short_writes=sum(0<=r[2]<r[1] for r in calls),
                write_calls=sum(r[0]=='write' for r in calls),pwrite_calls=sum(r[0] in ('pwrite','pwrite64') for r in calls))


def storage_summary(path, start, end):
    phases={}
    if not path.exists():return phases
    with path.open() as file:
        for row in csv.DictReader(file):
            a,b=int(row['start_us']),int(row['end_us'])
            if a>=end or b<=start:continue
            phases.setdefault(row['phase'],[]).append(row)
    result={}
    for phase,rows in phases.items():
        costs=[(int(r['end_us'])-int(r['start_us']))/1000 for r in rows]
        result[phase]=dict(calls=len(rows),errors=sum(int(r['result'])<0 for r in rows),absent=sum(int(r['result'])==1 for r in rows),
                           bytes=sum(int(r['bytes']) for r in rows),cpu_s=sum(int(r['cpu_us']) for r in rows)/1e6,
                           wall_s=sum(costs)/1000,p50_ms=quantile(costs,.5),p95_ms=quantile(costs,.95),
                           p99_ms=quantile(costs,.99),max_ms=max(costs))
    return result


def proc_sample(pid):
    base=Path(f'/proc/{pid}')
    fields=(base/'stat').read_text().rsplit(')',1)[1].split()
    status=dict(line.split(':',1) for line in (base/'status').read_text().splitlines() if ':' in line)
    if fields[0] in ('Z','X') or 'VmRSS' not in status or 'VmHWM' not in status:
        raise ProcessLookupError('process exited before memory sampling')
    thread_rows={}
    for task in (base/'task').iterdir():
        try:thread_rows[task.name]=dict(schedstat=(task/'schedstat').read_text().strip(),wchan=(task/'wchan').read_text().strip())
        except OSError:continue  # A single exiting thread does not end process sampling.
    return dict(time_us=time.monotonic_ns()//1000,
                cpu_s=(int(fields[11])+int(fields[12]))/os.sysconf('SC_CLK_TCK'),
                rss_kb=int(status['VmRSS'].split()[0]), hwm_kb=int(status['VmHWM'].split()[0]),
                process_io=(base/'io').read_text(), host_stat=Path('/proc/stat').read_text().splitlines()[0],
                loadavg=Path('/proc/loadavg').read_text().strip(),
                pressure={k:Path(f'/proc/pressure/{k}').read_text() for k in ('cpu','io','memory')},
                threads=thread_rows)


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


async def exercise(port, bots, cycles, item, result, warmup, idle, prepare_only, active_timeout=300):
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
            bot.latencies.clear();bot.failures=0;bot.acknowledged.clear()
        async def work(bot):
            for _ in range(cycles):
                if not await bot.block_cycle(bot.profile_target):break
                bot.cycles_completed+=1
                await asyncio.sleep(.001)
        start=now()
        if not prepare_only:await asyncio.wait_for(asyncio.gather(*(work(b) for b in actors)),active_timeout)
        else:await asyncio.sleep(.001)
        phases['active']=[start,now()]
        start=now();await asyncio.sleep(idle);phases['idle_after']=[start,now()]
        assert all(b.state=='play' and not b.error for b in actors), 'connection failure'
        latencies=[v for b in actors for v in b.latencies]
        report=dict(phases=phases,targets=[list(b.profile_target) for b in actors],bots=[b.report() for b in actors],
                    action_times=[dict(place_sent=list(b.place_send_times),break_sent=list(b.break_send_times),
                                       place_confirmed=list(b.place_verified_times),break_confirmed=list(b.break_verified_times),
                                       latencies_ms=list(b.latencies)) for b in actors],
                    pickup_observations=[dict(events=len(b.pickups),items=sum(n for _,n in b.pickups)) for b in actors],
                    pickup_events_first_observer=list(actors[0].pickups),
                    acknowledged_actions=[list(b.acknowledged) for b in actors],
                    acknowledged_place=sum(r["state"]!=0 for b in actors for r in b.acknowledged),
                    acknowledged_break=sum(r["state"]==0 for b in actors for r in b.acknowledged),
                    placed=sum(b.placed for b in actors),broken=sum(b.broken for b in actors),
                    missing=sum(cycles-b.cycles_completed for b in actors) if not prepare_only else 0,
                    failures=sum(b.failures for b in actors),
                    latencies_ms=dict(mean=statistics.mean(latencies) if latencies else None,
                                      p50=quantile(latencies,.5),p95=quantile(latencies,.95),p99=quantile(latencies,.99),
                                      maximum=max(latencies,default=None)))
        report['expected_final']=[dict(point=list(b.profile_target),state=0) for b in actors]
        report['final_validation_placements']=0
        if not prepare_only and item==1:
            # Outside the fixed-work timing: leave a NEW positive state, so a
            # restart cannot pass just by finding the original all-air target.
            async def sentinel(bot):
                x,y,z=bot.profile_target;bot.clear_block_events(bot.profile_target);bot.sequence+=1
                payload=varint(0)+position(x,y-1,z)+varint(1)+struct.pack('>fff??',.5,1.,.5,False,False)+varint(bot.sequence)
                await bot.conn.send(66,payload)
                state=await bot.expect_block(bot.profile_target,lambda value:value!=0)
                assert state is not None,'final durable sentinel not acknowledged'
                return dict(point=[x,y,z],state=state)
            started=now();report['expected_final']=await asyncio.gather(*(sentinel(b) for b in actors))
            report['phases']['durability_tail']=[started,now()]
            report['final_validation_placements']=len(actors)
        (result/'actions.json').write_text(json.dumps(report,indent=2))
        assert report['missing']==report['failures']==0, 'unconfirmed actions'
        assert report['acknowledged_place']==report['placed'] and report['acknowledged_break']==report['broken'], 'state/sequence ACK counts differ'
        return report
    finally:
        await asyncio.gather(*(close(b) for b in actors if b.conn),return_exceptions=True)
        bot_log.close()


async def verify_restart(binary, backend, save_mode, world, expected, result):
    folder=result/'verification';folder.mkdir()
    runtime=folder/'runtime';runtime.mkdir();copy=runtime/'world';shutil.copytree(world,copy)
    (runtime/'assets').symlink_to(ROOT/'assets')
    (runtime/'server.config').write_text(f'online-mode=false\noffline-skins=false\nview-distance=2\nsimulation-distance=2\nchunk-storage={backend}\n'+(f'anvil-save-mode={save_mode}\n' if save_mode!='immediate' else ''))
    with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    assert port not in (25565,25587)
    env=os.environ.copy()
    for name in ('LD_PRELOAD','MC_SAVE_TRACE_PATH','MC_PERF_TRACE_PATH','MC_FSYNC_TRACE_PATH','MC_PROC_FINAL_PATH','MC_IO_TRACE_PATH','MC_IO_TRACE_ROOT'):
        env.pop(name,None)
    env.update(MC_WORLD_PATH=str(copy),MC_BIND_PORT=str(port),MC_QA_MODE='1',MC_ADMIN_USERNAME='StressPROF00000',MC_PERF='0',MC_SETUP_FILE=str(ROOT/'tests/stress/network/one.conf'))
    opts=options(['--host','127.0.0.1','--port',str(port),'--mode','connect','--bots','1','--timeout','30']);opts.run_id='PROF'
    actor=ProfileBot(0,opts,lambda _:None);started=time.monotonic()
    with (folder/'server.log').open('w') as log:
        process=subprocess.Popen([str(binary)],cwd=runtime,env=env,stdout=log,stderr=subprocess.STDOUT)
        try:
            deadline=time.monotonic()+30
            while True:
                if process.poll() is not None:raise RuntimeError('restart startup failed')
                try:
                    with socket.create_connection(('127.0.0.1',port),timeout=.2):break
                except ConnectionRefusedError:
                    if time.monotonic()>deadline:raise TimeoutError('restart readiness')
                    await asyncio.sleep(.05)
            await actor.connect();await actor.conn.send(44)
            for point in expected:
                assert await query_block(actor,tuple(point['point']))==point['state'],point
        finally:
            if actor.conn:await close(actor)
            if process.poll() is None:process.send_signal(signal.SIGTERM)
            try:await asyncio.to_thread(process.wait,60)
            except subprocess.TimeoutExpired:
                process.kill();await asyncio.to_thread(process.wait);raise RuntimeError('restart shutdown timeout')
    assert process.returncode==0,'restart persistence failed'
    proof=dict(status='passed',targets=len(expected),seconds=time.monotonic()-started)
    (folder/'proof.json').write_text(json.dumps(proof,indent=2));return proof


async def one(args,binary,label,bots,result,template=None,prepare_only=False,backend="bin",save_mode="immediate"):
    result.mkdir(parents=True,exist_ok=False)
    runtime=result/'runtime';runtime.mkdir();world=runtime/'world'
    if template:shutil.copytree(template,world)
    (runtime/'assets').symlink_to(ROOT/'assets')
    (runtime/'server.config').write_text(f'online-mode=false\noffline-skins=false\nview-distance=10\nsimulation-distance=10\nlevel-seed=0\nchunk-storage={backend}\n')
    if save_mode!='immediate':
        with (runtime/'server.config').open('a') as config:config.write(f'anvil-save-mode={save_mode}\n')
    with socket.socket() as sock:
        sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    assert port not in (25565,25587)
    env=dict(os.environ,MC_WORLD_PATH=str(world),MC_BIND_PORT=str(port),
             MC_SETUP_FILE=str(ROOT/'tests/stress/network/one.conf'),MC_PERF='1',MC_PERF_SLOW_MS='50',
             MC_PERF_SUMMARY_TICKS='100',MC_PERF_TRACE_PATH=str(result/'ticks.csv'))
    if args.fsync_probe:
        env.update(LD_PRELOAD=str(args.fsync_probe.resolve()),MC_FSYNC_TRACE_PATH=str(result/'fsync.csv'),MC_PROC_FINAL_PATH=str(result/'process-final.json'))
    if args.write_trace:
        assert args.fsync_probe, 'write tracing requires the explicitly preloaded test probe'
        env.update(MC_IO_TRACE_PATH=str(result/'writes.csv'),MC_IO_TRACE_ROOT=str(world))
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
            report=await exercise(port,bots,args.cycles,args.item,result,args.warmup,args.idle,prepare_only,args.active_timeout)
        finally:
            shutdown_start=time.monotonic_ns()//1000
            if proc.poll() is None:proc.send_signal(signal.SIGTERM)
            try:await asyncio.to_thread(proc.wait,60)
            except subprocess.TimeoutExpired:
                proc.kill();await asyncio.to_thread(proc.wait);raise RuntimeError('unclean shutdown')
            finally:
                shutdown_end=time.monotonic_ns()//1000
                stop.set()
                try:await sampling
                finally:(result/'resources.json').write_text(json.dumps(samples,indent=2))
        assert proc.returncode==0, f'server exit {proc.returncode}'
    report['phases']['shutdown']=[shutdown_start,shutdown_end]
    (result/'actions.json').write_text(json.dumps(report,indent=2))
    server_log=(result/'server.log').read_text()
    assert 'world shutdown persistence FAILED' not in server_log and 'chunk store save failed' not in server_log and 'durability uncertain' not in server_log,'unconfirmed persistence'
    wanted={tuple(p['point']):p['state'] for p in report['expected_final']};restored=[]
    reverse={v:k for k,v in report_states(ROOT/'data/26.1.1/reports/blocks.json').items()}
    for (cx,cz),root,_ in chunks(world,backend):
        selected=[p for p in wanted if (p[0]//16,p[2]//16)==(cx,cz)]
        if not selected:continue
        sec=sections(root)
        for x,y,z in selected:
            states=section_blocks(sec[y//16],backend=='bin') if y//16 in sec else ['minecraft:air']*4096
            assert states[((y%16)*16+z%16)*16+x%16]==reverse[wanted[(x,y,z)]],(x,y,z,'not saved')
            restored.append([x,y,z]);wanted.pop((x,y,z))
    assert not wanted,('missing saved chunks',wanted)
    final_usage=json.loads((result/'process-final.json').read_text()) if args.fsync_probe else None
    if final_usage:
        assert final_usage['pid']==proc.pid
        assert final_usage['vmhwm_kb']>0
    all_writes=write_summary(result/'writes.csv',0,shutdown_end+1) if args.write_trace else None
    if all_writes:
        assert final_usage['write_trace_dropped']==0
        assert final_usage['world_write_calls']==all_writes['calls']
        assert final_usage['world_write_bytes']==all_writes['completed_bytes']
    (result/'durable-targets.json').write_text(json.dumps(dict(status='passed',targets=restored,boundary='clean shutdown with no persistence error; independent NBT block-state check'),indent=2))
    rows=read_trace(result/'ticks.csv')
    if args.fsync_probe:assert (result/'fsync.csv').is_file(), 'fsync probe did not produce a trace'
    assert rows[0]['start_us'] <= report['phases']['active'][0] and rows[-1]['end_us'] >= report['phases']['active'][1], 'active phase not fully traced'
    summaries={}
    for phase,(a,b) in report['phases'].items():
        if b<=a:continue
        stats=summarize(rows,a,b);stats['fsync']=io_summary(result/'fsync.csv',a,b) if args.fsync_probe else None
        if args.write_trace:stats['writes']=write_summary(result/'writes.csv',a,b)
        stats['storage']=storage_summary(result/'saves.csv',a,b)
        selected=[s for s in samples if a<=s['time_us']<=b]
        if len(selected)>1:
            stats['process_cpu_percent']=100*(selected[-1]['cpu_s']-selected[0]['cpu_s'])/((selected[-1]['time_us']-selected[0]['time_us'])/1e6)
            before_io={k:int(v) for k,v in (line.split(':') for line in selected[0]['process_io'].splitlines())}
            after_io={k:int(v) for k,v in (line.split(':') for line in selected[-1]['process_io'].splitlines())}
            stats['sampled_process_io_delta']={k:after_io[k]-v for k,v in before_io.items()}
            stats['client_cpu_percent']=100*(selected[-1]['client_cpu_s']-selected[0]['client_cpu_s'])/((selected[-1]['time_us']-selected[0]['time_us'])/1e6)
            stats['rss_min_kb']=min(s['rss_kb'] for s in selected);stats['rss_max_kb']=max(s['rss_kb'] for s in selected)
        summaries[phase]=stats
    verification=await verify_restart(binary,backend,save_mode,world,report['expected_final'],result) if not prepare_only else None
    result_row=dict(backend=backend,save_mode=save_mode,label=label,binary=str(binary),sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),bots=bots,
                    cycles_per_bot=args.cycles,item_id=args.item,port=port,phases=summaries,
                    placed=report['placed'],broken=report['broken'],missing=report['missing'],failures=report['failures'],
                    acknowledged_place=report['acknowledged_place'],acknowledged_break=report['acknowledged_break'],
                    final_validation_placements=report['final_validation_placements'],restart_verification=verification,
                    proposed_place=sum(len(r['place_sent']) for r in report['action_times']),
                    proposed_break=sum(len(r['break_sent']) for r in report['action_times']),
                    pickup_observations=report['pickup_observations'],
                    latencies_ms=report['latencies_ms'],total_including_shutdown_s=shutdown_end/1e6-started,
                    after_active_until_shutdown_s=(shutdown_end-report['phases']['active'][1])/1e6,
                    total_storage=storage_summary(result/'saves.csv',0,shutdown_end+1),
                    durable_targets=len(restored),final_process_usage=final_usage,world_write_trace=all_writes,
                    rss_peak_observed_kb=max([s['hwm_kb'] for s in samples]+[final_usage['vmhwm_kb'] if final_usage else 0]))
    (result/'summary.json').write_text(json.dumps(result_row,indent=2))
    print(json.dumps(dict(label=label,bots=bots,result=str(result),placed=report['placed'],broken=report['broken'],active=summaries['active'])),flush=True)
    return result_row


async def main(args):
    assert 0<args.active_timeout<=3600, 'active timeout must be bounded (0,3600] seconds'
    args.result_dir=args.result_dir.resolve();args.result_dir.mkdir(parents=True,exist_ok=False)
    binaries=[]
    for value in args.binary:
        label,path=value.split('=',1);binary=Path(path).resolve();assert binary.is_file()
        assert label and all(c.isalnum() or c in '-_' for c in label)
        binaries.append((label,binary))
    modes=dict(v.split('=',1) for v in args.storage_backend)
    assert set(modes)<=set(label for label,_ in binaries), 'unknown backend label'
    assert all(v in ('bin','anvil-atomic-experimental') for v in modes.values())
    policies=dict(v.split('=',1) for v in args.save_mode)
    assert set(policies)<=set(label for label,_ in binaries), 'unknown save-mode label'
    assert all(v in ('immediate','batched','async') for v in policies.values())
    assert all(v=='immediate' or modes.get(k,'bin')=='anvil-atomic-experimental' for k,v in policies.items())
    templates={};semantics={}
    for label,binary in binaries:
        backend=modes.get(label,'bin')
        if backend in templates:continue
        if args.world_template:
            assert len(set(modes.values())|{'bin'})==1, 'mixed backends require independent templates'
            template=args.world_template.resolve()
        else:
            await one(args,binary,'template',1,args.result_dir/f'template-{backend}',prepare_only=True,backend=backend)
            template=args.result_dir/f'template-{backend}/runtime/world'
        templates[backend]=template
        manifest={str(p.relative_to(template)):hashlib.sha256(p.read_bytes()).hexdigest() for p in template.rglob('*') if p.is_file()}
        (args.result_dir/f'template-manifest-{backend}.json').write_text(json.dumps(manifest,indent=2))
        semantics[backend]=world_blocks(template,backend)
        (args.result_dir/f'template-blocks-{backend}.json').write_text(json.dumps(semantics[backend],indent=2))
    assert all(s==next(iter(semantics.values())) for s in semantics.values()), 'template block states/work differ'
    rows=[]
    for bots in args.bots:
        for repetition in range(args.repetitions):
            order=binaries if repetition%2==0 else list(reversed(binaries))
            for label,binary in order:
                name=f'{label}-{bots}-r{repetition+1}'
                backend=modes.get(label,"bin")
                row=await one(args,binary,label,bots,args.result_dir/name,templates[backend],backend=backend,save_mode=policies.get(label,'immediate'))
                row['repetition']=repetition+1;rows.append(row)
                (args.result_dir/'comparison.json').write_text(json.dumps(rows,indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary',action='append',required=True,help='label=/absolute/path/to/binary')
    p.add_argument('--storage-backend',action='append',default=[],help='label=bin or label=anvil-atomic-experimental; independent equivalent templates')
    p.add_argument('--save-mode',action='append',default=[],help='label=immediate|batched|async; opt-in Anvil policy')
    p.add_argument('--bots',nargs='+',type=int,default=[1,10,25,50])
    p.add_argument('--repetitions',type=int,default=3)
    p.add_argument('--cycles',type=int,default=100)
    p.add_argument('--active-timeout',type=float,default=300,help='bounded fixed-work deadline; raise explicitly and identically across modes for slow HDD tests')
    p.add_argument('--item',type=int,default=1)
    p.add_argument('--warmup',type=float,default=5)
    p.add_argument('--idle',type=float,default=5)
    p.add_argument('--fsync-probe',type=Path)
    p.add_argument('--write-trace',action='store_true',help='test-probe write/pwrite calls under the disposable world; includes initialization and shutdown')
    p.add_argument('--save-trace',action='store_true',help='record storage stages and server action queue/handler times')
    p.add_argument('--world-template',type=Path)
    p.add_argument('--result-dir',type=Path,required=True)
    args=p.parse_args()
    if any(not 1<=n<=50 for n in args.bots) or args.cycles<1 or args.repetitions<1:p.error('positive cycles/repetitions and 1..50 bots required')
    if not all(math.isfinite(x) and x>=0 for x in (args.warmup,args.idle)):p.error('invalid phase duration')
    if args.fsync_probe and not args.fsync_probe.is_file():p.error('fsync probe library not found')
    asyncio.run(main(args))
