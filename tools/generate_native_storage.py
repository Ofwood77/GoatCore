#!/usr/bin/env python3
"""Generate opaque BE/POI guards from an exact-version official Java audit.

Audit: compile tools/audit_vanilla_storage.java against the official server and
libraries, run it, then pass --audit OUTPUT once to refresh the versioned facts.
No Java or raw Mojang reports are required to verify the generated C header.
"""
import argparse,hashlib,json,re
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def generate(facts):
 registry=(ROOT/'src/world/block_registry.c').read_text().split('const mc_block_properties_t GLOBAL_BLOCK_STATES')[0]
 names={name:int(index) for index,name in re.findall(r'\[(\d+)\] = \{"([^"]+)",',registry)}
 assert len(names)==facts['block_count']==1168
 out='/* Generated from official 26.1.1 BlockEntityType.isValid / PoiTypes.hasPoi.\n * tools/generate_native_storage.py; no runtime Java dependency. */\n'
 out+='static const struct { const char *entity; const char *block; } native_be_blocks[] = {\n'
 for entity,block in facts['pairs']:out+=f'    {{"{entity}", "{block}"}},\n'
 out+='};\nstatic const uint8_t native_sensitive_by_block[1168] = {\n'
 for name in sorted(set(facts['poi'])|{b for _,b in facts['pairs']}):out+=f'    [{names[name]}] = 1, /* {name} */\n'
 return out+'};\n'
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--audit',type=Path);p.add_argument('--server-jar',type=Path);p.add_argument('--check',action='store_true');a=p.parse_args()
 fixture=ROOT/'tests/fixtures/vanilla-26.1.1-native-storage.json';header=ROOT/'src/generated/generated_native_be_blocks.h'
 if a.audit:
  assert a.server_jar and hashlib.sha256(a.server_jar.read_bytes()).hexdigest()=='3647564632a2f589be2a87bf544f628e5f67cb3780c86f7fa972ec36892aa081','exact official 26.1.1 JAR required'
  lines=a.audit.read_text().splitlines();facts=dict(version='26.1.1',protocol=775,data_version=4788,block_count=1168,
   jar_sha256='3647564632a2f589be2a87bf544f628e5f67cb3780c86f7fa972ec36892aa081',
   pairs=sorted(l.split('\t')[1:] for l in lines if l.startswith('PAIR\t')),
   poi=sorted(l.split('\t')[1] for l in lines if l.startswith('POI\t')),
   statuses=[l.split('\t')[1] for l in lines if l.startswith('STATUS\t')])
  assert len(facts['statuses'])==12 and len(set(e for e,_ in facts['pairs']))==49
  fixture.write_text(json.dumps(facts,indent=2)+'\n')
 facts=json.loads(fixture.read_text());result=generate(facts)
 if a.check:assert header.read_text()==result,'native storage table differs; regenerate from official audit'
 else:header.write_text(result)
 print('native BE/POI storage table verified' if a.check else 'native BE/POI storage table generated')
