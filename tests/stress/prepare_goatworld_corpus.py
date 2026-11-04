#!/usr/bin/env python3
"""Read-only source inventory and explicitly labelled disposable G1 corpora."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import random
import struct
from native_storage_oracle import chunks,parse,value
from native_corpus import encode


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def integer(n):return (3,struct.pack('>i',n))
def historical_files(world):
    return {str(p.relative_to(world)):sha(p) for p in sorted((world/'dimensions/minecraft/overworld/region').glob('*.mca'))}
def put(folder,x,z,raw):
    (folder/f'c_{x}_{z}.nbt').write_bytes(raw)
def make(args):
    out=args.result_dir.resolve();out.mkdir(parents=True,exist_ok=False)
    history=args.historical_world.resolve();official=args.official_world.resolve()
    before={str(history):historical_files(history),str(official):historical_files(official)}
    actual=sorted(list(chunks(history)),key=lambda item:item[0]);assert len(actual)==483
    manifest=dict(schema=1,seed=7754788,source_region_files=before,source_not_written=True,corpora={})
    for count in (1,100,483):
        folder=out/str(count);folder.mkdir()
        selected=[max(actual,key=lambda v:len(v[2]))] if count==1 else [actual[i*len(actual)//count] for i in range(count)]
        for (x,z),root,raw in selected:put(folder,x,z,raw)
        manifest['corpora'][str(count)]=dict(kind='subset of real historical GoatCore runtime, S3C converted; no source relocation',chunks=count,raw_sha256={f'{x},{z}':hashlib.sha256(raw).hexdigest() for (x,z),root,raw in selected})
    full=out/'official-full';full.mkdir();protected=[];accepted=[];types={}
    for (x,z),root,raw in chunks(official):
        version=int.from_bytes(value(root,'DataVersion'),'big',signed=True)
        status=value(root,'Status',b'').decode('ascii')
        info=dict(x=x,z=z,version=version,status=status,raw_sha256=hashlib.sha256(raw).hexdigest())
        if version==4788 and status=='minecraft:full':
            put(full,x,z,raw);accepted.append(info)
            for be in value(root,'block_entities',(10,[]))[1]:
                kind=value(be,'id').decode('ascii');types[kind]=types.get(kind,0)+1
        else:protected.append(info)
    manifest['corpora']['official-full']=dict(kind='unmodified raw NBT from official 26.1.1 full chunks; no promotion of protochunks',chunks=len(accepted),accepted=accepted,protected_not_imported=protected,block_entity_types=types)
    report=json.loads(args.blocks_report.read_text())
    endings=('_wool','_concrete','_terracotta','_planks','_stairs','_slab','_log','_wood','_leaves','_bricks')
    palette=[]
    for name,block in sorted(report.items()):
        if not name.endswith(endings):continue
        state=next(s for s in block['states'] if s.get('default'))
        item={b'Name':(8,name.encode())}
        if state.get('properties'):item[b'Properties']=(10,{k.encode():(8,v.encode()) for k,v in sorted(state['properties'].items())})
        palette.append(item)
    assert len(palette)>=256;palette=palette[:256]
    def template(kind):
        sections=[]
        for sy in range(-4,20):
            sections.append({b'Y':(1,struct.pack('>b',sy)),b'biomes':(10,{b'palette':(9,(8,[b'minecraft:plains']))}),
                             b'block_states':(10,{b'palette':(9,(10,[{b'Name':(8,b'minecraft:stone' if sy<4 else b'minecraft:air')}]))})})
        if kind=='diverse':
            rng=random.Random(7754788)
            for sy in range(8,12):
                values=[rng.randrange(256) for _ in range(4096)]
                data=b''.join(struct.pack('>Q',sum(values[i+j]<<(8*j) for j in range(8))) for i in range(0,4096,8))
                sections[sy][b'block_states']=(10,{b'palette':(9,(10,copy.deepcopy(palette))),b'data':(12,data)})
        root={b'DataVersion':integer(4788),b'xPos':integer(0),b'zPos':integer(0),b'yPos':integer(-4),
              b'Status':(8,b'minecraft:full'),b'sections':(9,(10,sections)),b'block_entities':(9,(10,[])),
              b'block_ticks':(9,(10,[])),b'fluid_ticks':(9,(10,[])),b'SyntheticG1':(8,kind.encode())}
        if kind=='opaque':root[b'UnknownG1']=(10,{b'incompressible':(7,random.Random(775).randbytes(65536)),b'compressible':(7,bytes(range(256))*256),b'typed':(9,(10,[{b'number':(4,struct.pack('>q',-7754788))}]))})
        return root
    templates={k:template(k) for k in ('uniform','diverse','opaque')}
    for label,count,spread in [('1000',1000,False),('mixed32',32,False),('spread128',128,True)]:
        folder=out/label;folder.mkdir();kinds={}
        for i in range(count):
            kind=('uniform','uniform','diverse','opaque')[i%4];root=copy.deepcopy(templates[kind])
            x=(i%32)-32 if not spread else (i%16-8)*32
            z=(i//32)-32 if not spread else (i//16-4)*32
            root[b'xPos']=integer(x);root[b'zPos']=integer(z)
            raw=encode(root);put(folder,x,z,raw);kinds[kind]=kinds.get(kind,0)+1
        manifest['corpora'][label]=dict(kind='synthetic full NBT envelopes, never presented as official world generation',chunks=count,distribution=kinds,regions='128 one-chunk regions' if spread else 'dense negative-coordinate region',derived_data='not invented; absent height/light fields intentionally absent in synthetic envelope')
    for kind,root in templates.items():
        folder=out/kind;folder.mkdir();root[b'xPos']=integer(-1);root[b'zPos']=integer(1);put(folder,-1,1,encode(root))
        manifest['corpora'][kind]=dict(kind='synthetic standalone encoding case',chunks=1)
    for label,info in manifest['corpora'].items():
        files=sorted((out/label).glob('*.nbt'));info['raw_bytes']=sum(p.stat().st_size for p in files)
        info['file_manifest_sha256']=hashlib.sha256(b''.join(p.name.encode()+b'\0'+bytes.fromhex(sha(p)) for p in files)).hexdigest()
    assert historical_files(history)==before[str(history)] and historical_files(official)==before[str(official)]
    manifest['blocks_report_sha256']=sha(args.blocks_report)
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps({name:{k:v for k,v in data.items() if k in ('chunks','raw_bytes','distribution','block_entity_types')} for name,data in manifest['corpora'].items()}))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('historical-world','official-world','blocks-report','result-dir'):p.add_argument('--'+name,type=Path,required=True)
    make(p.parse_args())
