#!/usr/bin/env python3
"""Pause the REAL S4 writer at old snapshot A, mutate both hinged families,
then drain B and restart. Fault hooks exist only in mc_server_g2_fault.
Run remotely, on an explicitly supplied disposable GoatWorld template.
"""
import argparse
import asyncio
import json
import os
from pathlib import Path
import select
import shutil
import signal
import socket
import struct
import subprocess
from types import SimpleNamespace
from test_block_placement import Placer,command,hold,place,observed,report_states
from protocol import position,varint
from test_stair_placement import batch
from test_player_visibility import close,until
from test_block_persistence import query_block
from test_save_durability import ready,saved
from native_storage_oracle import chunks,sections,section_blocks
ROOT=Path(__file__).resolve().parents[2]
BACKEND='goatworld-sqlite-experimental';DOOR=(-8,200,24);TRAP=(-6,200,24)

def free_port():
    with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
    assert port not in (25565,25587);return port

async def run(a):
    out=a.result_dir.resolve();out.mkdir(parents=True,exist_ok=False)
    world=out/'world';shutil.copytree(a.template.resolve(),world)
    (out/'assets').symlink_to(ROOT/'assets')
    (out/'server.config').write_text('online-mode=false\noffline-skins=false\nview-distance=2\nchunk-storage='+BACKEND+'\nchunk-save-mode=async\n')
    states=report_states(ROOT/'data/26.1.1/reports/blocks.json');names={v:k for k,v in states.items()}
    ids=json.loads((ROOT/'src/generated/generated_minecraft_ids.json').read_text())
    items={i['name']:i['id'] for i in ids['items']};expected={}
    for first in (True,False):
        port=free_port();env=dict(os.environ,MC_WORLD_PATH=str(world),MC_BIND_PORT=str(port),MC_QA_MODE='1',MC_ADMIN_USERNAME='Ofwood_',MC_SETUP_FILE=str(ROOT/'tests/stress/network/one.conf'))
        r,w=os.pipe();c,d=os.pipe()
        if first:env.update(G1_TEST_PHASE='sql-before-commit',G1_TEST_READY=str(w),G1_TEST_RELEASE=str(c),G1_TEST_ARM_FILE=str(out/'armed'))
        bots=[]
        with (out/('gated.log' if first else 'restart.log')).open('w') as log:
            p=subprocess.Popen([str((a.fault_server if first else a.server).resolve())],cwd=out,env=env,pass_fds=(w,c) if first else (),stdout=log,stderr=subprocess.STDOUT)
            os.close(w);os.close(c)
            try:
                await ready(p,port)
                opts=SimpleNamespace(host='127.0.0.1',port=port,timeout=30,run_id='HAS',shared_zone=False,mode='connect',qa_verify_movement=False)
                bots=[Placer(0,opts),Placer(1,opts)];bots[0].name='Ofwood_';actor=bots[0]
                for b in bots:await b.connect();await b.conn.send(44)
                if first:
                    await batch(actor,[f'setblock {x} 199 {z} stone' for x,z in ((-11,25),(-13,25),(-8,24),(-6,24))])
                    for b,x in zip(bots,(-10.5,-12.5)):
                        await command(actor,f'tp {b.name} {x} 200 25.5')
                        await until(lambda:b.pos and abs(b.pos[0]-x)<.01,'teleport')
                        await b.conn.send(30,struct.pack('>ddd?',x,200.,25.5,True))
                        await asyncio.wait_for(b.chunk_ready_events[(-1,1)].wait(),30)
                    for point,item in ((DOOR,'oak_door'),(TRAP,'oak_trapdoor')):
                        await hold(actor,items['minecraft:'+item]);await place(actor,(point[0],199,point[2]),1,(.5,1.,.5))
                        value=await query_block(actor,point);assert 'open=false' in names[value]
                    await saved(actor);(out/'armed').write_text('after both closed generations durable\n')
                    await command(actor,'setblock -4 200 24 stone')
                    assert (await asyncio.to_thread(select.select,[r],[],[],30))[0] and os.read(r,1)==b'R','old writer not gated'
                    # Worker owns A with CLOSED blocks; tick is free to mutate B.
                    await actor.conn.send(56,struct.pack('>h',36)+b'\0')
                    for point in (DOOR,TRAP):
                        initial=await query_block(actor,point);opened=states[names[initial].replace('open=false','open=true')]
                        for b in bots:b.clear_block_events(point)
                        await place(actor,point,1,(.5,.5,.5));await asyncio.gather(*(observed(b,point,opened) for b in bots))
                        assert await query_block(actor,point)==opened
                        expected[point]=opened
                        # Replayed main-hand sequence and related off-hand use
                        # must not toggle the accepted transition a second time.
                        replay=varint(0)+position(*point)+varint(1)+struct.pack('>fff??',.5,.5,.5,False,False)+varint(actor.sequence)
                        await actor.conn.send(66,replay);await asyncio.sleep(.1)
                        assert await query_block(actor,point)==opened
                        actor.sequence+=1
                        await actor.conn.send(66,varint(1)+position(*point)+varint(1)+struct.pack('>fff??',.5,.5,.5,False,False)+varint(actor.sequence))
                        await asyncio.sleep(.1);assert await query_block(actor,point)==opened
                    # Two independent players may interact in the same fast pass.
                    # Each accepted action toggles once; their final state is open.
                    await asyncio.gather(*(place(b,DOOR,1,(.5,.5,.5)) for b in bots))
                    assert await query_block(actor,DOOR)==expected[DOOR]
                    expected[(DOOR[0],201,DOOR[2])]=states[names[expected[DOOR]].replace('half=lower','half=upper')]
                    # Disarm before releasing A so B can commit normally.
                    (out/'armed').unlink();os.write(d,b'G');await saved(actor)
                else:
                    for point,value in expected.items():assert await query_block(actor,point)==value,(point,'new generation lost')
                    await saved(actor)
            finally:
                if first and (out/'armed').exists():(out/'armed').unlink();os.write(d,b'G')
                await asyncio.gather(*(close(b) for b in bots if b.conn),return_exceptions=True)
                if p.poll() is None:p.send_signal(signal.SIGTERM)
                try:await asyncio.to_thread(p.wait,60)
                except subprocess.TimeoutExpired:p.kill();await asyncio.to_thread(p.wait);raise
                os.close(r);os.close(d)
            assert p.returncode==0
    actual={}
    for (cx,cz),root,_ in chunks(world,BACKEND):
        for point in expected:
            x,y,z=point
            if (x//16,z//16)==(cx,cz):actual[point]=section_blocks(sections(root)[y//16])[((y%16)*16+z%16)*16+x%16]
    assert actual=={p:names[v] for p,v in expected.items()}
    proof=dict(status='passed',old_snapshot='closed',new_snapshot='open',writer_gate='sql-before-commit',two_observers=True,normal_restart=True,independent_sqlite_nbt=True,states=[dict(point=list(p),state=v) for p,v in actual.items()])
    (out/'proof.json').write_text(json.dumps(proof,indent=2)+'\n');print(json.dumps(proof))
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('server','fault-server','template','result-dir'):p.add_argument('--'+name,type=Path,required=True)
    asyncio.run(run(p.parse_args()))
