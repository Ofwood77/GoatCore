#!/usr/bin/env python3
"""Independent NBT/padded-palette assertions for disposable S3A test worlds.

Does not write worlds. It deliberately distinguishes legacy compact .bin data
from Vanilla SimpleBitStorage and hashes every block, including implicit air.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

from check_region_storage import reference


class NBT:
    def __init__(self, data):
        self.data=data;self.pos=0

    def take(self, n):
        assert n>=0 and self.pos+n<=len(self.data)
        result=self.data[self.pos:self.pos+n];self.pos+=n;return result

    def number(self, fmt):
        return struct.unpack('>'+fmt,self.take(struct.calcsize('>'+fmt)))[0]

    def text(self):
        # Reference worlds use standard ASCII names/properties. Keep unknown
        # strings as bytes so modified-UTF8 cannot be normalized accidentally.
        return self.take(self.number('H'))

    def value(self, kind, depth=0):
        assert depth<128
        if kind in range(1,7):return self.take({1:1,2:2,3:4,4:8,5:4,6:8}[kind])
        if kind==8:return self.text()
        if kind in (7,11,12):return self.take(self.number('i')*{7:1,11:4,12:8}[kind])
        if kind==9:
            elem=self.number('B');n=self.number('i');assert n>=0
            return elem,[self.value(elem,depth+1) for _ in range(n)]
        if kind==10:
            values={}
            while True:
                t=self.number('B')
                if t==0:break
                name=self.text();assert name not in values
                values[name]=(t,self.value(t,depth+1))
            return values
        raise ValueError(f'unsupported NBT type {kind}')


def parse(raw):
    reader=NBT(raw);assert reader.number('B')==10
    reader.text();result=reader.value(10);assert reader.pos==len(raw)
    return result


def value(node, name, default=None):
    field=node.get(name.encode());return field[1] if field else default


def integer(node, name):return int.from_bytes(value(node,name),'big',signed=True)


def state(entry):
    name=value(entry,'Name').decode('ascii')
    props=value(entry,'Properties',{})
    return name+('['+','.join(k.decode('ascii')+'='+v[1].decode('ascii') for k,v in sorted(props.items()))+']' if props else '')


def section_blocks(section, compact=False):
    bs=value(section,'block_states')
    if bs is None:return ['minecraft:air']*4096
    kind,palette=value(bs,'palette');assert kind==10 and palette
    keys=[state(e) for e in palette]
    if len(keys)==1:return keys*4096
    bits=max(4,(len(keys)-1).bit_length());packed=value(bs,'data')
    words=[int.from_bytes(packed[i:i+8],'big') for i in range(0,len(packed),8)]
    out=[]
    for i in range(4096):
        if compact:
            bit=i*bits;at,shift=divmod(bit,64);index=words[at]>>shift
            if shift+bits>64:index|=words[at+1]<<(64-shift)
        else:
            per=64//bits;index=words[i//per]>>((i%per)*bits)
        index&=(1<<bits)-1;assert index<len(keys);out.append(keys[index])
    return out


def sections(root):
    kind,items=value(root,'sections');assert kind==10
    return {integer(s,'Y'):s for s in items}


def block_digest(root, compact=False):
    sec=sections(root);digest=hashlib.sha256()
    for sy in range(-4,20):
        for key in section_blocks(sec[sy],compact) if sy in sec else ['minecraft:air']*4096:
            digest.update(key.encode()+b'\0')
    return digest.hexdigest()


def chunks(world, backend='anvil-atomic-experimental'):
    if backend=='bin':
        for path in sorted((world/'chunks').glob('*.bin')):
            raw=zlib.decompress(path.read_bytes(),wbits=47);root=parse(raw)
            yield (integer(root,'xPos'),integer(root,'zPos')),root,raw
    elif backend=='goatworld-sqlite-experimental':
        import sqlite3,ctypes
        lib=ctypes.CDLL('liblz4.so.1')
        decode=lib.LZ4_decompress_safe
        decode.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_int,ctypes.c_int];decode.restype=ctypes.c_int
        db=sqlite3.connect((world/'goatworld/chunks.sqlite').resolve().as_uri()+'?mode=ro',uri=True)
        try:
            assert db.execute('PRAGMA application_id').fetchone()[0]==1196896561
            assert db.execute('PRAGMA user_version').fetchone()[0]==1
            assert db.execute('PRAGMA integrity_check').fetchone()==('ok',)
            for d,x,z,g,c,f,level,n,digest,payload in db.execute('SELECT d,x,z,g,c,f,l,n,sha,payload FROM chunks ORDER BY d,x,z'):
                assert d==0 and 0<n<=8388608 and (c,f,level)==(2,0,0) and g>0
                out=ctypes.create_string_buffer(n);assert decode(payload,out,len(payload),n)==n
                raw=out.raw;metadata=bytearray(80)
                struct.pack_into('>IiiIQIII',metadata,0,d,x,z,n,g,c,f,level)
                metadata[40:72]=hashlib.sha256(raw).digest();assert hashlib.sha256(metadata).digest()==digest
                root=parse(raw);assert (integer(root,'xPos'),integer(root,'zPos'),integer(root,'DataVersion'))==(x,z,4788)
                yield (x,z),root,raw
        finally:db.close()
    else:
        for path in sorted((world/'dimensions/minecraft/overworld/region').glob('*.mca')):
            _,_,decoded=reference(path)
            for _,raw in sorted(decoded.items()):
                root=parse(raw);yield (integer(root,'xPos'),integer(root,'zPos')),root,raw


def world_blocks(world, backend):
    return {f'{x},{z}':block_digest(root,backend=='bin') for (x,z),root,_ in chunks(world,backend)}


def normalize(root):
    """Only block palettes and derived height/light data may change in S3A."""
    import copy
    out=copy.deepcopy(root)
    out.pop(b'isLightOn',None)
    hm=value(out,'Heightmaps',{})
    for key in ('WORLD_SURFACE_WG','WORLD_SURFACE','OCEAN_FLOOR_WG','OCEAN_FLOOR','MOTION_BLOCKING','MOTION_BLOCKING_NO_LEAVES'):
        hm.pop(key.encode(),None)
    for section in sections(out).values():
        for name in (b'BlockLight',b'SkyLight',b'block_states'):section.pop(name,None)
    return out


def preservation(before, after):
    old={xy:(r,raw) for xy,r,raw in chunks(before)}
    new={xy:(r,raw) for xy,r,raw in chunks(after)}
    assert old.keys()<=new.keys()
    changed=[]
    for xy,(root,raw) in old.items():
        other,other_raw=new[xy]
        if raw==other_raw:continue
        # Existing palette entries (and any unknown entry fields) retain order.
        ss=sections(other)
        for sy,s in sections(root).items():
            bs=value(s,'block_states')
            if bs:
                oldp=value(bs,'palette')[1];newp=value(value(ss[sy],'block_states'),'palette')[1]
                assert newp[:len(oldp)]==oldp,(xy,sy,'palette preservation')
        assert normalize(root)==normalize(other),(xy,'unknown NBT changed')
        changed.append(xy)
    # GoatCore has no global world/POI/entity writer in S3A.
    files=[]
    for p in before.rglob('*'):
        if p.is_file() and (p.name.startswith('level.dat') or 'entities' in p.parts or 'poi' in p.parts or p.relative_to(before).parts[0] in ('data','datapacks')):
            relative=p.relative_to(before);assert p.read_bytes()==(after/relative).read_bytes(),relative
            files.append(str(relative))
    return dict(original_chunks=len(old),final_chunks=len(new),changed_chunks=changed,unchanged_chunks=len(old)-len(changed),preserved_global_files=files)


def same_blocks(before, after, selected=None):
    old={xy:root for xy,root,_ in chunks(before)}
    new={xy:root for xy,root,_ in chunks(after)}
    keys=selected if selected is not None else sorted(old)
    assert keys
    hashes={}
    for xy in keys:
        assert xy in old and xy in new,xy
        assert integer(old[xy],'DataVersion')==integer(new[xy],'DataVersion')==4788
        digest=block_digest(old[xy]);assert digest==block_digest(new[xy]),xy
        hashes[str(xy)]=digest
    return dict(status='passed',chunks=len(keys),block_count_per_chunk=98304,hashes=hashes)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('world',type=Path)
    p.add_argument('--backend',choices=('bin','anvil-atomic-experimental'),default='anvil-atomic-experimental')
    p.add_argument('--preserved-from',type=Path);p.add_argument('--output',type=Path)
    p.add_argument('--same-blocks-as',type=Path);p.add_argument('--chunk',action='append',help='optional x,z selection for same-blocks-as')
    args=p.parse_args()
    if args.same_blocks_as:
        selected=[tuple(map(int,xy.split(','))) for xy in args.chunk] if args.chunk else None
        result=same_blocks(args.same_blocks_as,args.world,selected)
    else:result=preservation(args.preserved_from,args.world) if args.preserved_from else world_blocks(args.world,args.backend)
    text=json.dumps(result,indent=2)
    if args.output:args.output.write_text(text+'\n')
    else:print(text)
