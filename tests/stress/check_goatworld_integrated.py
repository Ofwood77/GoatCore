#!/usr/bin/env python3
"""Remote-only G2 Minecraft validation. Fresh worlds and explicitly seeded test fixtures."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
from native_storage_oracle import chunks,value
ROOT=Path(__file__).resolve().parents[2]

def port():
 with socket.socket() as s:s.bind(('127.0.0.1',0));n=s.getsockname()[1]
 assert n not in (25565,25587);return n

def run(a):
 out=a.result_dir.resolve();out.mkdir(parents=True,exist_ok=False)
 runner=[sys.executable,ROOT/'tests/stress/run_block_checks.py']
 def execute(tag,cmd,env=None):
  with (out/(tag+'.log')).open('w') as log:subprocess.run(list(map(str,cmd)),stdout=log,stderr=subprocess.STDOUT,env=env,check=True,timeout=900,cwd=ROOT)
 base=['--kind','goatcore','--server-binary',a.server.resolve(),'--chunk-storage','goatworld-sqlite-experimental','--anvil-save-mode','async','--view-distance','2','--verify-restarts','2']
 for suite,extra in [('placement',['--blocks-only','--region-boundaries']),('stairs',[]),('pickup',['--pickup-count','100','--view-distance','10'])]:
  execute(suite,runner+base+['--suite',suite,'--port',port(),'--result-dir',out/suite,*extra])
 execute('vanilla-stairs',runner+['--kind','vanilla','--suite','stairs','--java',a.java.resolve(),'--vanilla-root',a.vanilla_root.resolve(),'--port',port(),'--shutdown-timeout','180','--result-dir',out/'vanilla-stairs'])
 gc=json.loads((out/'stairs/report-edit.json').read_text());vanilla=json.loads((out/'vanilla-stairs/report-edit.json').read_text())
 assert gc['cases']==vanilla['cases'] and len(gc['cases'])==192
 # The test-only existing S3B probe seeds one opaque official NBT fixture. It is
 # not an importer, never runs on live or personal worlds, and creates no MCA.
 fixture=out/'complex-fixture';fixture.mkdir();(fixture/'.goatcore-s3b-disposable').write_text('G2 test fixture only\n')
 raw=a.complex_nbt.read_bytes();execute('complex-seed',[a.seed_tool.resolve(),'--seed',fixture,-1,1,a.complex_nbt.resolve()],env=dict(os.environ,MC_TEST_STORAGE='goatworld-sqlite-experimental'))
 seeded={xy:(root,data) for xy,root,data in chunks(fixture,'goatworld-sqlite-experimental')};assert seeded[(-1,1)][1]==raw
 execute('complex',runner+base+['--suite','complex','--port',port(),'--world-template',fixture,'--result-dir',out/'complex'])
 final={xy:root for xy,root,data in chunks(out/'complex/runtime/world','goatworld-sqlite-experimental')}
 old=value(seeded[(-1,1)][0],'block_entities');new=value(final[(-1,1)],'block_entities');assert old==new and len(old[1])==7
 types=[value(entity,'id').decode() for entity in old[1]]
 proof=dict(status='passed',stairs_observations=192,block_entities=types,nbt_types_values_exact=True,seed_nbt_sha256=hashlib.sha256(raw).hexdigest(),server_sha256=hashlib.sha256(a.server.read_bytes()).hexdigest(),new_worlds=True,automatic_conversion=False)
 (out/'proof.json').write_text(json.dumps(proof,indent=2)+'\n');print(json.dumps(proof))
if __name__=='__main__':
 p=argparse.ArgumentParser()
 for key in ('server','seed-tool','complex-nbt','java','vanilla-root','result-dir'):p.add_argument('--'+key,type=Path,required=True)
 run(p.parse_args())
