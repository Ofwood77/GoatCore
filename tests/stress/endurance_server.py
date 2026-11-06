#!/usr/bin/env python3
"""Test-only server supervisor. FIFO trace sinks rotate without truncating live FDs."""
import argparse
from collections import defaultdict, deque
import csv
import json
import os
from pathlib import Path
import signal
import subprocess
import threading
import time
from endurance_common import Rotating, atomic_json, utc
from profile_tick import summarize


def run(args):
    root=args.campaign.resolve(); root.mkdir(parents=True,exist_ok=True)
    ticks=deque(maxlen=2400); saves=defaultdict(lambda:dict(calls=0,bytes=0,wall_us=0,cpu_us=0,errors=0,max_wall_us=0))
    lock=threading.Lock(); faults=[]; child=None; stopping=[None]
    def reader(kind):
        try:
            path=root/(kind+'.fifo')
            if path.exists():path.unlink() # only this known campaign FIFO, before child startup
            os.mkfifo(path,0o600)
            fd=os.open(path,os.O_RDWR)
            with os.fdopen(fd) as source:
                sink=Rotating(root/(kind+'.csv'))
                header=next(source);sink.write(header)
                fields=next(csv.reader([header]))
                for line in source:
                    sink.write(line)
                    row=dict(zip(fields,next(csv.reader([line]))))
                    with lock:
                        if kind=='ticks':ticks.append({k:int(v) for k,v in row.items()})
                        else:
                            value=saves[row['phase']];value['calls']+=1;value['bytes']+=int(row['bytes'])
                            value['wall_us']+=int(row['end_us'])-int(row['start_us']);value['cpu_us']+=int(row['cpu_us'])
                            value['errors']+=int(row['result'])!=0
                            value['max_wall_us']=max(value['max_wall_us'],int(row['end_us'])-int(row['start_us']))
                            value['last_cx']=int(row['cx']);value['last_cz']=int(row['cz'])
        except BaseException as exc:
            faults.append(repr(exc))
    for kind in ('ticks','saves'):
        threading.Thread(target=reader,args=(kind,),daemon=True).start()
    deadline=time.monotonic()+10
    while not all((root/(k+'.fifo')).exists() for k in ('ticks','saves')):
        if time.monotonic()>deadline:raise TimeoutError('trace sinks')
        time.sleep(.01)
    env=dict(os.environ,MC_PERF='1',MC_PERF_TRACE_PATH=str(root/'ticks.fifo'),MC_SAVE_TRACE_PATH=str(root/'saves.fifo'))
    child=subprocess.Popen([args.binary.resolve()],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,bufsize=1)
    def stop(*_):
        if stopping[0] is None:stopping[0]=time.monotonic()
        if child.poll() is None:child.send_signal(signal.SIGTERM)
    signal.signal(signal.SIGTERM,stop);signal.signal(signal.SIGINT,stop)
    def logs():
        try:
            sink=Rotating(root/'server.log')
            for line in child.stdout:sink.write(line)
            sink.close()
        except BaseException as exc:faults.append(repr(exc))
    threading.Thread(target=logs,daemon=True).start()
    try:
        atomic_json(root/'server-process.json',dict(pid=child.pid,started_utc=utc()))
        while child.poll() is None:
            if faults:stop()
            with lock:
                rows=list(ticks); trace={k:dict(v) for k,v in saves.items()}
            end=rows[-1]['end_us'] if rows else 0
            tick=summarize(rows,max(rows[0]['start_us'],end-30_000_000),end) if len(rows)>1 else None
            atomic_json(root/'server-metrics.json',dict(utc=utc(),pid=child.pid,tick=tick,
                total_ticks=rows[-1]['idx']+1 if rows else 0,tick_end_monotonic_us=end,saves=trace,trace_errors=faults))
            time.sleep(2)
    finally:
        if child.poll() is None:
            stop()
            try:child.wait(timeout=150)
            except subprocess.TimeoutExpired:child.kill();child.wait()
    atomic_json(root/'server-exit.json',dict(utc=utc(),returncode=child.returncode,trace_errors=faults,shutdown_s=time.monotonic()-stopping[0] if stopping[0] else None))
    return child.returncode or (1 if faults else 0)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--binary',type=Path,required=True);p.add_argument('--campaign',type=Path,required=True)
    raise SystemExit(run(p.parse_args()))
