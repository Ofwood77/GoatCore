#!/usr/bin/env python3
"""Real server SIGKILL: visible mutation versus a durable SQLite generation."""
import argparse
import asyncio
import json
import os
from pathlib import Path
import select
import shutil
import signal
import socket
import subprocess
from types import SimpleNamespace
from test_block_placement import Placer,command,observed,report_states
from test_player_visibility import close
from test_block_persistence import query_block
from test_save_durability import ready,saved
from native_storage_oracle import chunks,sections,section_blocks,normalize,value
ROOT=Path(__file__).resolve().parents[2];POINT=(-8,200,24);BACKEND='goatworld-sqlite-experimental'
def port():
 with socket.socket() as s:s.bind(('127.0.0.1',0));p=s.getsockname()[1]
 assert p not in (25565,25587);return p

def state(world):
 for xy,root,raw in chunks(world,BACKEND):
  if xy==(-1,1):return section_blocks(sections(root)[200//16])[((200%16)*16+24%16)*16+(-8)%16]
 raise AssertionError('missing persisted chunk')

async def scenario(a,out,phase):
 runtime=out/phase;runtime.mkdir();world=runtime/'world';shutil.copytree(a.template.resolve(),world)
 (runtime/'assets').symlink_to(ROOT/'assets');(runtime/'server.config').write_text('online-mode=false\noffline-skins=false\nview-distance=2\nchunk-storage='+BACKEND+'\nchunk-save-mode=async\n')
 assert state(world)=='minecraft:air'
 original={xy:root for xy,root,raw in chunks(world,BACKEND)}
 states=report_states(ROOT/'data/26.1.1/reports/blocks.json');proof={}
 for fault in (True,False):
  number=port();env=dict(os.environ,MC_WORLD_PATH=str(world),MC_BIND_PORT=str(number),MC_SETUP_FILE=str(ROOT/'tests/stress/network/one.conf'),MC_QA_MODE='1',MC_ADMIN_USERNAME='Ofwood_')
  for k in ('LD_PRELOAD','G1_TEST_PHASE','G1_TEST_READY','G1_TEST_RELEASE','G1_TEST_ARM_FILE'):env.pop(k,None)
  r,w=os.pipe();c,d=os.pipe()
  if fault:env.update(G1_TEST_PHASE=phase,G1_TEST_READY=str(w),G1_TEST_RELEASE=str(c),G1_TEST_ARM_FILE=str(runtime/'armed'))
  binary=a.fault_server if fault else a.server;bots=[]
  with (runtime/('fault.log' if fault else 'restart.log')).open('w') as log:
   p=subprocess.Popen([str(binary.resolve())],cwd=runtime,env=env,pass_fds=(w,c) if fault else (),stdout=log,stderr=subprocess.STDOUT)
   os.close(w);os.close(c)
   try:
    await ready(p,number)
    opts=SimpleNamespace(host='127.0.0.1',port=number,timeout=30,run_id='G2CR',shared_zone=False,mode='connect',qa_verify_movement=False)
    bots=[Placer(0,opts),Placer(1,opts)];bots[0].name='Ofwood_'
    for b in bots:await b.connect();await b.conn.send(44)
    for b,x in zip(bots,(-11.5,-13.5)):
     await command(bots[0],f'tp {b.name} {x} 64 26.5');await asyncio.wait_for(b.chunk_ready_events[(-1,1)].wait(),30)
    if fault:
     await saved(bots[0]);(runtime/'armed').write_text('armed after initial drain\n')
     for b in bots:b.clear_block_events(POINT)
     await command(bots[0],'setblock -8 200 24 minecraft:stone')
     await asyncio.gather(*(observed(b,POINT,states['minecraft:stone']) for b in bots))
     assert (await asyncio.to_thread(select.select,[r],[],[],30))[0] and os.read(r,1)==b'R'
     assert await query_block(bots[0],POINT)==states['minecraft:stone']
     p.kill();await asyncio.to_thread(p.wait,10);assert p.returncode==-9
     proof['visible_before_kill']='minecraft:stone';proof['signal']='SIGKILL'
    else:
     expected='minecraft:air' if phase=='sql-before-commit' else 'minecraft:stone'
     # QA is deliberately administrator-only. The second bot is an observer;
     # it must not be granted privileges or used for an unauthorized query.
     assert await query_block(bots[0],POINT)==states[expected]
     await saved(bots[0]);proof['restored']=expected
   finally:
    await asyncio.gather(*(close(b) for b in bots if b.conn),return_exceptions=True)
    if p.poll() is None:
     p.kill() if fault else p.send_signal(signal.SIGTERM)
     try:await asyncio.to_thread(p.wait,60)
     except subprocess.TimeoutExpired:p.kill();await asyncio.to_thread(p.wait);raise
    os.close(r);os.close(d)
   if not fault:assert p.returncode==0
 assert state(world)==proof['restored']
 restored={xy:root for xy,root,raw in chunks(world,BACKEND)}
 for xy,root in original.items():assert normalize(root)==normalize(restored[xy]),(xy,'opaque NBT changed')
 entities=sum(len(value(root,'block_entities',(0,[]))[1]) for root in original.values())
 proof.update(status='passed',observers=2,phase=phase,independent_sqlite_nbt_check=True,
              opaque_nbt_types_values_exact=True,preserved_block_entities=entities)
 (runtime/'proof.json').write_text(json.dumps(proof,indent=2)+'\n');return proof

async def run(a):
 out=a.result_dir.resolve();out.mkdir(parents=True,exist_ok=False)
 results=[await scenario(a,out,phase) for phase in ('sql-before-commit','durable')]
 (out/'proof.json').write_text(json.dumps(dict(status='passed',scenarios=results),indent=2)+'\n')
if __name__=='__main__':
 p=argparse.ArgumentParser()
 for key in ('server','fault-server','template','result-dir'):p.add_argument('--'+key,type=Path,required=True)
 asyncio.run(run(p.parse_args()))
