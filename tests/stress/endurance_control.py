#!/usr/bin/env python3
"""Remote campaign controller. No credentials, no writes to the public service."""
import argparse
import asyncio
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import statistics
import struct
import subprocess
import sys
import time
import threading
import urllib.request
from endurance_common import Rotating, atomic_json, proc, protection, utc
from protocol import string, varint

HERE=Path(__file__).resolve().parent


def public_snapshot():
    text=subprocess.check_output(['systemctl','show','goatcore.service','-p','MainPID','-p','ActiveState','-p','NRestarts'],text=True,timeout=5)
    files=('/opt/goatcore/app/mc_server','/opt/goatcore/app/instances/online/server.config','/etc/goatcore/server1.setup.conf','/etc/systemd/system/goatcore.service')
    return dict(unit=dict(line.split('=',1) for line in text.splitlines()),
        files={p:hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in files})


def public_latency():
    started=time.monotonic()
    def number(sock):
        value=0
        for i in range(5):
            byte=sock.recv(1)
            if not byte:raise OSError('status EOF')
            value|=(byte[0]&127)<<(i*7)
            if byte[0]<128:return value
        raise ValueError('status framing')
    with socket.create_connection(('127.0.0.1',25565),2) as sock:
        sock.settimeout(2)
        body=varint(0)+varint(775)+string('127.0.0.1')+struct.pack('>H',25565)+varint(1)
        sock.sendall(varint(len(body))+body+b'\1\0')
        size=number(sock)
        if not 0<size<1048576:raise ValueError('status bound')
        received=0
        while received<size:
            part=sock.recv(min(65536,size-received))
            if not part:raise OSError('short status')
            received+=len(part)
    return (time.monotonic()-started)*1000


def published_sha():
    # Public read-only API, no token/SSH key/credential helper on the test host.
    request=urllib.request.Request('https://api.github.com/repos/Ofwood77/GoatCore/git/ref/heads/main',
        headers={'Accept':'application/vnd.github+json','User-Agent':'GoatCore-endurance'})
    with urllib.request.urlopen(request,timeout=10) as response:
        raw=response.read(65537)
    if len(raw)>65536:raise ValueError('GitHub response bound')
    sha=json.loads(raw)['object']['sha']
    if len(sha)!=40 or any(c not in '0123456789abcdef' for c in sha):raise ValueError('GitHub SHA')
    return sha


def cgroup(unit):
    name=subprocess.check_output(['systemctl','show',unit,'-p','ControlGroup','--value'],text=True,timeout=5).strip()
    root=Path('/sys/fs/cgroup')/name.lstrip('/')
    return {n:(root/n).read_text().strip() for n in ('memory.current','memory.peak','memory.max','memory.events','memory.pressure','memory.swap.current','cpu.stat') if (root/n).exists()}


def full_pressure(path):
    for line in Path(path).read_text().splitlines():
        if line.startswith('full '):return float(dict(part.split('=') for part in line.split()[1:])['avg10'])
    return 0


def world_size(path):
    files=list(path.rglob('*'));sizes={str(p.relative_to(path)):p.stat().st_size for p in files if p.is_file()}
    return sum(sizes.values()),sum(v for k,v in sizes.items() if k.endswith('-wal')),sizes


async def run(args):
    config=json.loads(args.config.read_text());root=Path(config['campaign']);root.mkdir(parents=True,exist_ok=True)
    assert config['port']==25587 and config['published_sha'] and config['ramp'][0]==5
    assert config['ramp']==sorted(set(config['ramp'])) and config['ramp'][-1]<=250
    baseline=public_snapshot();assert baseline['unit']['ActiveState']=='active'
    manifest=root/'manifest.json'
    if manifest.exists():raise FileExistsError('campaign already started; choose a new campaign ID')
    binary=Path(config['binary']);checksum=hashlib.sha256(binary.read_bytes()).hexdigest()
    assert checksum==config['binary_sha256'],'test binary differs from configuration'
    script_files=[*HERE.glob('endurance_*.py'),HERE/'protocol.py',HERE/'stress.py',HERE/'profile_tick.py']
    script_hashes={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in script_files}
    samples=[public_latency() for _ in range(5)]
    if max(samples)>config['limits']['public_latency_ms']/2:raise RuntimeError('public baseline too slow for safe load')
    atomic_json(manifest,dict(config=config,started_utc=utc(),baseline=baseline,public_baseline_latency_ms=samples,
        generator_sha256=script_hashes['endurance_bots.py'],script_hashes=script_hashes,
        published_commit_start=config['published_sha'],revision_role='published-origin-main' if config['watch_origin'] else 'uncommitted-validation-source-base',detection='next publicly observable origin/main SHA, not local commits'))
    events=Rotating(root/'events.jsonl',1048576,3);metrics=Rotating(root/'metrics.jsonl',8*1048576,6)
    stop=asyncio.Event();loop=asyncio.get_running_loop()
    for sig in (signal.SIGTERM,signal.SIGINT):loop.add_signal_handler(sig,stop.set)
    children=[];restart_counts={};started=time.monotonic();last_stage=started;stage=0;target=config['ramp'][0]
    previous={};bad=Counter();last_git=0;git_failures=0;reason='manual-stop';ramp=[];version_checks=[]
    def event(kind,**data):events.write(json.dumps(dict(utc=utc(),kind=kind,**data))+'\n')
    def spawn(first,count,folder,origin=None):
        process=subprocess.Popen([sys.executable,HERE/'endurance_bots.py','--first',str(first),'--count',str(count),
            '--period',str(config['period_s']),'--admission-origin',str(first if origin is None else origin),
            '--admission-interval',str(config.get('admission_interval_s',1)),'--output',folder],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        def logs():
            sink=Rotating(folder/'worker.log',1048576,2)
            for line in process.stdout:sink.write(line)
            sink.close()
        threading.Thread(target=logs,daemon=True).start()
        return dict(process=process,first=first,count=count,folder=folder)
    try:
        while not stop.is_set():
            if (root/'STOP').exists():reason='manual-stop-file';break
            allocated=sum(c['count'] for c in children);admission_origin=allocated
            while allocated<target:
                count=min(25,target-allocated);folder=root/f'worker-{allocated:03d}';folder.mkdir(exist_ok=True)
                children.append(spawn(allocated,count,folder,admission_origin));allocated+=count
                await asyncio.sleep(.5)
            statuses=[]
            for child in children:
                process=child['process']
                if process.poll() is not None:
                    key=child['first'];restart_counts[key]=restart_counts.get(key,0)+1
                    event('generator-exit',first=key,returncode=process.returncode,restart=restart_counts[key])
                    if restart_counts[key]>2:reason='generator-restart-limit';stop.set();break
                    await asyncio.sleep(5*restart_counts[key]);child.update(spawn(key,child['count'],child['folder']))
                path=child['folder']/'status.json'
                if path.exists():statuses.append(json.loads(path.read_text()))
            if stop.is_set():break
            try:
                public_now=public_snapshot();latency=public_latency()
            except Exception as exc:
                public_now={};latency=2000;event('public-probe-error',error=repr(exc))
            server=json.loads((root/'server-metrics.json').read_text()) if (root/'server-metrics.json').exists() else {}
            server_info=json.loads((root/'server-process.json').read_text()) if (root/'server-process.json').exists() else {}
            process_rows=[]
            for pid in [server_info.get('pid'),*[c['process'].pid for c in children]]:
                if not pid:continue
                try:process_rows.append(proc(pid))
                except (FileNotFoundError,ProcessLookupError):pass
            stamp=time.monotonic()
            for row in process_rows:
                old=previous.get(row['pid']);row['cpu_percent']=100*(row['cpu_s']-old['cpu_s'])/(stamp-old['stamp']) if old else None
                previous[row['pid']]=dict(cpu_s=row['cpu_s'],stamp=stamp)
            groups={unit:cgroup(unit) for unit in ('goatcore-test.service',config['generator_unit'])}
            available=int(next(line.split()[1] for line in Path('/proc/meminfo').read_text().splitlines() if line.startswith('MemAvailable:')))//1024
            disk=os.statvfs(root);disk_free=disk.f_bavail*disk.f_frsize//1048576
            size,wal,sizes=world_size(Path(config['world']))
            counts=Counter()
            for status in statuses:counts.update(status['counts'])
            active=sum(s['active'] for s in statuses);connected=sum(s['connected'] for s in statuses)
            observed=dict(public_unchanged=public_now==baseline,public_latency_ms=latency,available_mib=available,
                disk_free_mib=disk_free,world_bytes=size,wal_bytes=wal,
                oom=any(any(int(v) for k,v in (line.split() for line in g.get('memory.events','').splitlines()) if k in ('oom','oom_kill')) for g in groups.values()),
                memory_full_avg10=full_pressure('/proc/pressure/memory'),io_full_avg10=full_pressure('/proc/pressure/io'),
                worker_stale=any(stamp-s['monotonic']>30 for s in statuses) or (stamp-started>90 and len(statuses)!=len(children)),
                storage_errors=sum(v['errors'] for k,v in server.get('saves',{}).items() if k in ('sqlite_commit','sqlite_transaction','sqlite_checkpoint','player_file_save'))>0)
            reasons=protection(observed,config['limits'])
            if server.get('trace_errors'):reasons.append('trace-reader-failed')
            if stamp-started>90 and stamp-server.get('tick_end_monotonic_us',0)/1e6>60:reasons.append('test-tick-stalled')
            if (root/'server-exit.json').exists():reasons.append('test-server-exited')
            if hashlib.sha256(binary.read_bytes()).hexdigest()!=checksum:reasons.append('test-binary-changed')
            if any(hashlib.sha256((HERE/name).read_bytes()).hexdigest()!=sha for name,sha in script_hashes.items()):reasons.append('generator-code-changed')
            tick=server.get('tick')
            if tick and tick['tps']<10 and stamp-started>120:reasons.append('test-tps-under-10')
            if counts['permanent_failures']:reasons.append('permanent-bot-error')
            if counts['failed_actions']:reasons.append('unconfirmed-action-error')
            for key in list(bad):
                if key not in reasons:bad[key]=0
            for key in reasons:bad[key]+=1
            severe={'public-service-changed','cgroup-oom','storage-error','generator-stale','trace-reader-failed','test-tick-stalled','test-server-exited','test-binary-changed','generator-code-changed','permanent-bot-error','unconfirmed-action-error','disk-free','world-growth','wal-growth'}
            fatal=[key for key in reasons if key in severe or bad[key]>=3]
            row=dict(utc=utc(),elapsed_s=stamp-started,target=target,connected=connected,active=active,
                world_ready=sum(s['world_ready'] for s in statuses),inflight_actions=sum(s['inflight_actions'] for s in statuses),counts=dict(counts),server=server,
                processes=process_rows,cgroups=groups,guard=observed,guard_warnings=reasons,
                generator_network=[s['network'] for s in statuses],latencies=[s['latency_ms'] for s in statuses],
                world_files=sizes,diskstats=Path('/proc/diskstats').read_text(),loadavg=Path('/proc/loadavg').read_text(),
                system_cpu=Path('/proc/stat').read_text().split('intr ')[0],network_interfaces=Path('/proc/net/dev').read_text())
            metrics.write(json.dumps(row)+'\n');atomic_json(root/'latest.json',row)
            if fatal:reason=','.join(fatal);event('safety-stop',reasons=fatal);break
            if stamp-last_git>=120 and config['watch_origin']:
                last_git=stamp
                try:
                    sha=await asyncio.to_thread(published_sha);git_failures=0
                    version_checks.append(dict(utc=utc(),sha=sha));version_checks=version_checks[-8:]
                    atomic_json(root/'version-checks.json',version_checks)
                    if sha!=config['published_sha']:reason='published-origin-main-changed';event('version-change',sha=sha);break
                except Exception as exc:
                    git_failures+=1;event('version-probe-error',error=repr(exc),failures=git_failures)
                    if git_failures>=5:reason='version-watch-unavailable';break
            if stamp-last_stage>=config['plateau_s'] and active==target and not reasons and target not in [r['bots'] for r in ramp]:
                ramp.append(dict(bots=target,utc=utc(),metrics=row))
                atomic_json(root/'ramp.json',ramp);event('plateau-qualified',bots=target)
                if stage+1<len(config['ramp']):stage+=1;target=config['ramp'][stage];last_stage=stamp
            if stamp-last_stage>config['plateau_timeout_s'] and active<target:
                reason='activity-plateau-timeout';break
            if config.get('maximum_duration_s') and stamp-started>config['maximum_duration_s']:reason='bounded-validation-complete';break
            await asyncio.sleep(5)
    except BaseException as exc:
        reason='controller-error:'+repr(exc);event('controller-error',error=repr(exc));raise
    finally:
        for child in children:
            if child['process'].poll() is None:child['process'].terminate()
        async def reap(child):
            try:await asyncio.to_thread(child['process'].wait,30)
            except subprocess.TimeoutExpired:child['process'].kill();await asyncio.to_thread(child['process'].wait)
        await asyncio.gather(*(reap(child) for child in children))
        # Stop ONLY the explicitly authorized test instance after clients close;
        # its native shutdown flushes player files and drains S4.
        stopping=time.monotonic()
        result=subprocess.run(['systemctl','stop','goatcore-test.service'],check=False,timeout=180)
        exit_file=root/'server-exit.json'
        server_exit=json.loads(exit_file.read_text()) if exit_file.exists() else None
        atomic_json(root/'final.json',dict(utc=utc(),reason=reason,elapsed_s=time.monotonic()-started,
            final_target=target,qualified_plateaus=[r['bots'] for r in ramp],restart_counts=restart_counts,
            all_generators_stopped=True,test_stop_elapsed_s=time.monotonic()-stopping,
            test_stop_returncode=result.returncode,server_exit=server_exit))
        event('stopped',reason=reason);metrics.close();events.close()
    return 0 if server_exit and server_exit['returncode']==0 else 1


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--config',type=Path,required=True)
    raise SystemExit(asyncio.run(run(p.parse_args())))
