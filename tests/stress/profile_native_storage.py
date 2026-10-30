#!/usr/bin/env python3
"""S3B fixed-work data conservation cost; disposable worlds, no gameplay/TPS claim."""
import argparse,csv,hashlib,json,os,shutil,subprocess
from pathlib import Path
from profile_tick import quantile
from native_storage_oracle import preservation

def summary(rows):
 out={}
 for phase in ('load','encode','save'):
  selected=[r for r in rows if r.get('phase')==phase];wall=[r['ns']/1e6 for r in selected]
  out[phase]=dict(calls=len(selected),p50_ms=quantile(wall,.5),p95_ms=quantile(wall,.95),p99_ms=quantile(wall,.99),max_ms=max(wall),wall_s=sum(wall)/1000,cpu_s=sum(r['cpu_ns'] for r in selected)/1e9)
 out['rss_max_kb']=max(r['rss_kb'] for r in rows if 'rss_kb' in r)
 return out

def run(a):
 result=a.result_dir.resolve();result.mkdir(parents=True,exist_ok=False);cases=json.loads(a.cases.read_text());runs=[]
 variants=[('S3A-zero',a.baseline_probe,next(c for c in cases if c['name']=='zero'))]+[(f'S3B-{c["name"]}',a.probe,c) for c in cases]
 for rep in range(a.repetitions):
  order=variants if rep%2==0 else list(reversed(variants))
  for label,probe,case in order:
   folder=result/f'{label}-r{rep+1}';folder.mkdir();world=folder/'world';source=Path(case['world']);shutil.copytree(source,world)
   env=dict(os.environ,MC_SAVE_TRACE_PATH=str(folder/'saves.csv'))
   if a.fsync_probe:env.update(LD_PRELOAD=str(a.fsync_probe.resolve()),MC_FSYNC_TRACE_PATH=str(folder/'fsync.csv'))
   done=subprocess.run([str(probe.resolve()),str(world),*map(str,case['chunk']),'8','200','8',str(a.writes)],capture_output=True,text=True,env=env)
   (folder/'stdout.jsonl').write_text(done.stdout);(folder/'stderr.log').write_text(done.stderr);assert done.returncode==0,(label,done.stderr)
   rows=[json.loads(l) for l in done.stdout.splitlines()];assert next(r['saved'] for r in rows if 'saved' in r)==a.writes
   proof=preservation(source,world);(folder/'preservation.json').write_text(json.dumps(proof,indent=2))
   stats=summary(rows);phases=list(csv.DictReader((folder/'saves.csv').open()))
   stats['logical_write_bytes']=sum(int(r['bytes']) for r in phases if r['phase']=='region_total')
   assert sum(r['phase']=='region_total' for r in phases)==a.writes
   spans=[(int(r['start_us']),int(r['end_us'])) for r in phases if r['phase']=='region_total']
   inside=lambda r:any(a<=int(r['start_us']) and int(r['end_us'])<=b for a,b in spans)
   stats['fsync_region_file']=sum(r['phase']=='atomic_file_sync' and inside(r) for r in phases)
   stats['fsync_parent']=sum(r['phase']=='atomic_directory_sync' and inside(r) for r in phases)
   assert stats['fsync_region_file']==stats['fsync_parent']==a.writes
   if a.fsync_probe:
    calls=list(csv.reader((folder/'fsync.csv').open()));assert all(int(r[4])==0 for r in calls);stats['fsync_all']=len(calls)
   r=dict(label=label,repetition=rep+1,probe_sha256=hashlib.sha256(probe.read_bytes()).hexdigest(),stats=stats)
   runs.append(r);(result/'runs.json').write_text(json.dumps(runs,indent=2));print(json.dumps(dict(label=label,rep=rep+1,save=stats['save'],bytes=stats['logical_write_bytes'])),flush=True)
 (result/'summary.json').write_text(json.dumps({label:summary([json.loads(line) for r in runs if r['label']==label for line in (result/f'{label}-r{r["repetition"]}'/'stdout.jsonl').read_text().splitlines()]) for label,_,_ in variants},indent=2))
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('probe','baseline-probe','cases','result-dir'):p.add_argument('--'+name,type=Path,required=True)
 p.add_argument('--fsync-probe',type=Path);p.add_argument('--writes',type=int,default=100);p.add_argument('--repetitions',type=int,default=3)
 run(p.parse_args())
