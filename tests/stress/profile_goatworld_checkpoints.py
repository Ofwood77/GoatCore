#!/usr/bin/env python3
"""G2 checkpoint threshold comparison, fixed 2,000 two-chunk transactions."""
import argparse
import csv
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
from profile_tick import quantile

def run(a):
 out=a.result_dir.resolve();out.mkdir(parents=True,exist_ok=False);results=[]
 for rep in range(3):
  for pages in ((1000,4096) if rep%2==0 else (4096,1000)):
   tag=f'p{pages}-r{rep}';world=out/(tag+'-world');world.mkdir();(world/'.goatcore-g2-disposable').write_text('test only\n')
   subprocess.run([a.tool,'--disposable','seed',world],check=True,capture_output=True,timeout=30)
   trace=out/(tag+'.csv');env=dict(os.environ,MC_GOATWORLD_CHECKPOINT_PAGES=str(pages),MC_SAVE_TRACE_PATH=str(trace))
   started=time.monotonic()
   p=subprocess.run([a.tool,'--disposable','wal',world],capture_output=True,text=True,env=env,timeout=180)
   elapsed=time.monotonic()-started;(out/(tag+'.stdout')).write_text(p.stdout);(out/(tag+'.stderr')).write_text(p.stderr)
   assert p.returncode==0,p.stderr
   restored=json.loads(subprocess.check_output([a.tool,'--disposable','read',world],text=True).splitlines()[-1]);assert restored==dict(generations=[2001,2001],states=[1,1])
   with trace.open() as f:rows=list(csv.DictReader(f))
   commits=[x for x in rows if x['phase']=='sqlite_commit'];checkpoints=[x for x in rows if x['phase']=='sqlite_checkpoint'];assert len(commits)==2000
   ms=[(int(x['end_us'])-int(x['start_us']))/1000 for x in commits]
   result=dict(pages=pages,repetition=rep,transactions=2000,chunk_updates=4000,wall_s=elapsed,commit_p50_ms=quantile(ms,.5),commit_p95_ms=quantile(ms,.95),commit_p99_ms=quantile(ms,.99),
    checkpoints=[dict(log_pages=int(x['cx']),checkpointed_pages=int(x['cz']),mode=int(x['bytes']),wall_ms=(int(x['end_us'])-int(x['start_us']))/1000,result=int(x['result'])) for x in checkpoints],
    wal_committed_frame_highwater=max(int(x['cz']) for x in rows if x['phase']=='sqlite_transaction'),restored=restored)
   results.append(result);(out/'comparison.json').write_text(json.dumps(results,indent=2)+'\n');shutil.rmtree(world)
 print(json.dumps(dict(status='passed',runs=6,transactions=12000,verified_chunk_updates=24000)))
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--tool',type=Path,required=True);p.add_argument('--result-dir',type=Path,required=True);a=p.parse_args();a.tool=a.tool.resolve();run(a)
