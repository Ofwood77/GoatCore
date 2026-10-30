#!/usr/bin/env python3
"""S3B disposable corpus variants and fail-closed checks (no server starts)."""
import argparse,copy,hashlib,json,random,shutil,struct,subprocess
from pathlib import Path
from native_storage_oracle import chunks,value,integer
from native_corpus import seed

def digest(world):
 return {str(p.relative_to(world)):hashlib.sha256(p.read_bytes()).hexdigest() for p in world.rglob('*') if p.is_file() and p.suffix in ('.mca','.mcc','.bin')}
def run(a):
 result=a.result_dir.resolve();result.mkdir(parents=True,exist_ok=False)
 roots={xy:root for xy,root,_ in chunks(a.reference)};xy=(-1,1)
 def clone(name,source=a.reference):
  d=result/name;d.mkdir();world=d/'world';shutil.copytree(source,world);(world/'.goatcore-s3b-disposable').write_text('S3B disposable test only\n');return d,world
 cases=[]
 for name,kind in [('zero','official chunk without BEs'),('few','official chunk with seven BEs'),('many','composed from official dense chunk'),('opaque','synthetic opaque fields added to an official chunk')]:
  d,world=clone(name);coord=(-2,1) if name=='zero' else xy
  if name in ('many','opaque'):
   if name=='many':root=next(r for k,r,_ in chunks(a.dense) if k==xy)
   else:
    root=copy.deepcopy(roots[xy]);random_bytes=random.Random(7754788).randbytes(32768)
    opaque=(10,{b'bytes':(7,random_bytes),b'nested':(9,(10,[{b'value':(4,struct.pack('>q',-1234567890123)),b'text':(8,b'unknown')}]))})
    root[b'S3B_opaque']=opaque;value(root,'block_entities')[1][0][b'S3B_opaque']=copy.deepcopy(opaque)
   seed(a.probe,world,*xy,root,d)
  cases.append(dict(name=name,kind=kind,world=str(world),chunk=coord))
 refused=[]
 variants=['unknown-be','missing-id','bad-coordinates','wrong-block','bad-items','bad-components','duplicate-be','proto-synthetic','ticks-readonly','active-furnace-readonly','invalid-nbt','unknown-compression','corrupt-region','truncated-region']
 for name in variants:
  d,world=clone('refuse-'+name);root=copy.deepcopy(roots[xy]);be=value(root,'block_entities')[1][0]
  if name=='unknown-be':be[b'id']=(8,b'minecraft:s3b_unknown')
  elif name=='missing-id':be.pop(b'id')
  elif name=='bad-coordinates':be[b'x']=(3,struct.pack('>i',64))
  elif name=='wrong-block':be[b'id']=(8,b'minecraft:furnace')
  elif name=='bad-items':be[b'Items']=(3,struct.pack('>i',7))
  elif name=='bad-components':be[b'components']=(8,b'invalid')
  elif name=='duplicate-be':value(root,'block_entities')[1].append(copy.deepcopy(be))
  elif name=='proto-synthetic':root[b'Status']=(8,b'minecraft:carvers')
  elif name=='ticks-readonly':root[b'block_ticks']=(9,(10,[{b'i':(8,b'minecraft:stone'),b't':(3,struct.pack('>i',20)),b'p':(3,struct.pack('>i',0)),b'x':(3,struct.pack('>i',-8)),b'y':(3,struct.pack('>i',200)),b'z':(3,struct.pack('>i',24))}]))
  if name=='active-furnace-readonly':value(root,'block_entities')[1][1][b'lit_time_remaining']=(2,struct.pack('>h',100))
  seed(a.probe,world,*xy,root,d)
  region=world/'dimensions/minecraft/overworld/region/r.-1.0.mca'
  if name in ('invalid-nbt','unknown-compression'):
   data=bytearray(region.read_bytes());slot=31+32;offset=int.from_bytes(data[slot*4:slot*4+4],'big')>>8
   if name=='unknown-compression':data[offset*4096+4]=127
   else:
    raw=d/'invalid.nbt';raw.write_bytes(b'bad NBT')
    subprocess.run([str(a.probe.resolve()),'--seed',str(world),'-1','1',str(raw)],check=True)
   if name=='unknown-compression':region.write_bytes(data)
  if name=='corrupt-region':
   data=bytearray(region.read_bytes());data[63*4:63*4+4]=(257).to_bytes(4,'big');region.write_bytes(data)
  if name=='truncated-region':region.write_bytes(region.read_bytes()[:-1])
  before=digest(world)
  p=subprocess.run([str(a.probe.resolve()),str(world),'-1','1','8','200','8','1'],capture_output=True,text=True)
  (d/'probe.log').write_text(p.stdout+p.stderr);assert p.returncode!=0,(name,'not refused');assert digest(world)==before,(name,'changed source');assert not (world/'chunks').exists()
  refused.append(dict(case=name,exit=p.returncode,unchanged=True,kind='synthetic'))
 # A real protochunk, without forging its status or missing fields.
 real=next((k for k,r,_ in chunks(a.normal) if value(r,'Status')!=b'minecraft:full'),None)
 assert real
 d,world=clone('refuse-official-proto',a.normal);before=digest(world)
 p=subprocess.run([str(a.probe.resolve()),str(world),str(real[0]),str(real[1]),'8','200','8','1'],capture_output=True,text=True)
 (d/'probe.log').write_text(p.stdout+p.stderr);assert p.returncode!=0 and digest(world)==before
 refused.append(dict(case='official-proto',chunk=real,exit=p.returncode,unchanged=True,kind='official'))
 known=next(k for k,r,_ in chunks(a.normal) if value(r,'Status')==b'minecraft:full' and any(value(e,'id') in (b'minecraft:beehive',b'minecraft:mob_spawner') for e in value(r,'block_entities',(0,[]))[1]))
 d,world=clone('readonly-official-uncovered',a.normal);before=digest(world)
 p=subprocess.run([str(a.probe.resolve()),str(world),str(known[0]),str(known[1]),'8','200','8','1'],capture_output=True,text=True)
 (d/'probe.log').write_text(p.stdout+p.stderr);assert p.returncode!=0 and p.stdout.count('"phase":"load"')==10 and digest(world)==before
 refused.append(dict(case='official-uncovered-readonly',chunk=known,unchanged=True,readable=True,kind='official'))
 for missing in (False,True):
  d,world=clone('external-missing' if missing else 'external-present',a.external)
  if missing:
   external=world/'dimensions/minecraft/overworld/region/c.-1.1.mcc';external.unlink()
  before=digest(world);p=subprocess.run([str(a.probe.resolve()),str(world),'-1','1','8','200','8','1'],capture_output=True,text=True)
  (d/'probe.log').write_text(p.stdout+p.stderr);assert p.returncode!=0 and digest(world)==before
  refused.append(dict(case='external-missing' if missing else 'external-present',exit=p.returncode,unchanged=True,kind='synthetic official RegionFile API'))
 (result/'cases.json').write_text(json.dumps(cases,indent=2));(result/'refusals.json').write_text(json.dumps(refused,indent=2));print(json.dumps(dict(cases=len(cases),refusals=len(refused),status='passed')))
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('probe','reference','dense','normal','external','result-dir'):p.add_argument('--'+name,type=Path,required=True)
 run(p.parse_args())
