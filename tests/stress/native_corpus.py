#!/usr/bin/env python3
"""Read-only official corpus catalog and explicit synthetic disposable variants.

The catalog does not decode external .mcc data or follow its symlinks. Seeded
variants are labelled synthetic/composed, never claimed as natural generation.
"""
import argparse,collections,hashlib,json,os,stat,struct,subprocess,shutil
from pathlib import Path
from native_storage_oracle import parse,value,integer,chunks
from check_region_storage import reference

def payload(kind,v):
 if kind in range(1,9):
  if kind in range(1,7):return v
  if kind==7:return struct.pack('>i',len(v))+v
  return struct.pack('>H',len(v))+v
 if kind in (11,12):return struct.pack('>i',len(v)//(4 if kind==11 else 8))+v
 if kind==9:
  elem,items=v;return bytes([elem])+struct.pack('>i',len(items))+b''.join(payload(elem,t) for t in items)
 if kind==10:return b''.join(bytes([t])+struct.pack('>H',len(k))+k+payload(t,v) for k,(t,v) in v.items())+b'\0'
 raise ValueError(kind)
def encode(root):return b'\x0a\0\0'+payload(10,root)
def catalog(world):
 result=dict(regions={},statuses=collections.Counter(),entities=collections.Counter(),chunks=0,external=[],unsupported=[],errors=[])
 for path in sorted((world/'dimensions/minecraft/overworld/region').glob('*.mca')):
  data=path.read_bytes();entry,used,decoded=reference(path)
  region=dict(sha256=hashlib.sha256(data).hexdigest(),bytes=len(data),chunks=len(entry),occupied_sectors=sum(used))
  result['regions'][path.name]=region
  rx,rz=map(int,path.name.split('.')[1:3])
  for slot,e in entry.items():
   if e['codec']&128:
    cx,cz=rx*32+slot%32,rz*32+slot//32;external=path.parent/f'c.{cx}.{cz}.mcc'
    try:
     st=external.lstat();ok=stat.S_ISREG(st.st_mode) and 0<st.st_size<=64*1024*1024
     result['external'].append(dict(file=external.name,regular_bounded=ok,bytes=st.st_size,decoded=False))
    except FileNotFoundError:result['external'].append(dict(file=external.name,missing=True,decoded=False))
   elif e['codec'] not in (1,2,3):result['unsupported'].append(dict(file=path.name,slot=slot,codec=e['codec']))
  for slot,raw in decoded.items():
   try:
    root=parse(raw);status=value(root,'Status',b'unknown').decode();result['statuses'][status]+=1;result['chunks']+=1
    for be in value(root,'block_entities',(0,[]))[1]:result['entities'][value(be,'id',b'unknown').decode()]+=1
   except Exception as error:result['errors'].append(dict(region=path.name,slot=slot,error=str(error)))
 return result

def seed(probe,world,cx,cz,root,result):
 marker=world/'.goatcore-s3b-disposable';marker.write_text('S3B disposable composed/synthetic fixture\n')
 raw=encode(root);file=result/'seed.nbt';file.write_bytes(raw)
 subprocess.run([str(probe.resolve()),'--seed',str(world.resolve()),str(cx),str(cz),str(file.resolve())],check=True)
 return hashlib.sha256(raw).hexdigest()

def compare_entities(before,after,selected=None):
 def collect(world):
  out={}
  for xy,root,_ in chunks(world):
   if selected is not None and xy not in selected:continue
   for entity in value(root,'block_entities',(0,[]))[1]:
    key=(xy,tuple(integer(entity,k) for k in ('x','y','z')))
    assert key not in out,'duplicate BE position'
    out[key]=entity
  return out
 old,new=collect(before),collect(after)
 assert old==new,{'missing':list(old.keys()-new.keys()),'added':list(new.keys()-old.keys()),'changed':[k for k in old.keys()&new.keys() if old[k]!=new[k]]}
 return dict(status='passed',entities=len(old),types=dict(collections.Counter(value(e,'id').decode() for e in old.values())),comparison='exact NBT types and values; compound order immaterial')

if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('world',type=Path);p.add_argument('--output',type=Path);p.add_argument('--compare-entities',type=Path);p.add_argument('--chunk',action='append')
 a=p.parse_args()
 selected=[tuple(map(int,k.split(','))) for k in a.chunk] if a.chunk else None
 result=compare_entities(a.compare_entities,a.world,selected) if a.compare_entities else catalog(a.world)
 text=json.dumps(result,indent=2)
 if a.output:a.output.write_text(text+'\n')
 else:print(text)
