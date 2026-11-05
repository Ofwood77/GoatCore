#!/usr/bin/env python3
"""Fixed-work remote support-loss load; network ACK + both final block states.
Per cycle: support placement, attachment placement, support destruction. Natural
item drops are retained. S4 drain, disk oracle and a fresh restart are included.
"""
import argparse
import asyncio
import json
import os
from pathlib import Path
import struct
import time
import profile_tick as base
from protocol import position,varint,string,read_varint
from stress import options
from test_player_visibility import close,until
from test_block_placement import report_states
from test_stair_placement import batch
BACKEND='goatworld-sqlite-experimental'

class AttachedProfileBot(base.ProfileBot):
    def __init__(self,*args):
        super().__init__(*args);self.item_spawns=set();self.item_entity=None
    async def play_packet(self,packet_id,payload):
        if packet_id==1:
            entity,at=read_varint(payload);kind,_=read_varint(payload,at+16)
            if kind==self.item_entity:self.item_spawns.add(entity)
        await super().play_packet(packet_id,payload)

async def exercise(port,bots,cycles,item,result,warmup,idle,prepare_only,active_timeout=300):
    assert not prepare_only,'supply an existing disposable flat template'
    opts=options(['--host','127.0.0.1','--port',str(port),'--mode','blocks','--bots',str(bots),'--allow-block-changes','--timeout','60'])
    opts.run_id='ATPR';opts.place_limiter=None;opts.break_limiter=None
    actors=[AttachedProfileBot(i,opts,lambda _:None) for i in range(bots)]
    item_entity=next(i['id'] for i in json.loads((base.ROOT/'src/generated/generated_minecraft_ids.json').read_text())['entity_types'] if i['name']=='minecraft:item')
    for b in actors:b.item_entity=item_entity
    states=report_states(base.ROOT/'data/26.1.1/reports/blocks.json')
    items={i['name']:i['id'] for i in json.loads((base.ROOT/'src/generated/generated_minecraft_ids.json').read_text())['items']}
    material=('torch','soul_torch','copper_torch','lantern','ladder')
    sent=[dict(place_sent=[],break_sent=[]) for _ in actors];phases={};latencies=[];placed=broken=removed=0
    now=lambda:time.monotonic_ns()//1000
    async def action(bot,kind,expected):
        t=time.monotonic();support=bot.target;attachment=bot.attachment
        point=attachment if kind=='attachment' else support
        for p in (point,attachment):bot.clear_block_events(p)
        bot.sequence+=1
        sent[bot.index]['break_sent' if kind=='break' else 'place_sent'].append(now())
        if kind=='break':payload=varint(0)+position(*point)+varint(1)+varint(bot.sequence);packet=41
        else:
            slot=1 if kind=='attachment' else 0;await bot.conn.send(53,struct.pack('>h',slot))
            face=(5 if bot.index%5==4 else 1) if kind=='attachment' else 1
            clicked=support if kind=='attachment' else (support[0],support[1]-1,support[2])
            payload=varint(0)+position(*clicked)+varint(face)+struct.pack('>fff??',.5,.5,.5,False,False)+varint(bot.sequence);packet=66
        await bot.conn.send(packet,payload)
        assert await bot.expect_block(point,lambda s:s==expected) is not None,(bot.index,kind,'state/ACK')
        if kind=='break':assert await bot.expect_block(attachment,lambda s:s==0) is not None,(bot.index,'support loss')
        latencies.append((time.monotonic()-t)*1000)
    try:
        start=now()
        for b in actors:
            await b.connect()
            deadline=time.monotonic()+120
            while sum(e.is_set() for e in b.chunk_ready_events.values())<25:
                if time.monotonic()>deadline:raise TimeoutError('25 initial chunks')
                await asyncio.sleep(.05)
            b.target=(-8+(b.index%10)*8,64,24+(b.index//10)*8)
            x,y,z=b.target;b.attachment=(x+1,y,z) if b.index%5==4 else (x,y+1,z)
            await actors[0].conn.send(7,string(f'tp {b.name} {x-2.5} {y} {z+.5}'))
            await until(lambda:b.pos and abs(b.pos[0]-(x-2.5))<.01 and abs(b.pos[2]-z-.5)<.01,'private lane teleport',timeout=30)
            await b.conn.send(30,struct.pack('>ddd?',x-2.5,y,z+.5,True))
            await asyncio.wait_for(asyncio.gather(*(b.chunk_ready_events[(cx,cz)].wait() for cx in range(x//16-1,x//16+2) for cz in range(z//16-1,z//16+2))),60)
            for slot,name in ((36,'stone'),(37,material[b.index%5])):
                await b.conn.send(56,struct.pack('>h',slot)+varint(64)+varint(items['minecraft:'+name])+b'\0\0')
            # Preparation is outside the measured action phase: equivalent,
            # explicit flat private lanes, independent of terrain generation.
            commands=[f'setblock {x+dx} {y+dy} {z+dz} air' for dx in range(-3,3) for dz in range(-1,2) for dy in range(3)]
            commands += [f'setblock {x+dx} {y-1} {z+dz} stone' for dx in range(-3,3) for dz in range(-1,2)]
            await batch(actors[0],commands)
            name=material[b.index%5];key='minecraft:'+name
            if name=='lantern':key+='[hanging=false,waterlogged=false]'
            if name=='ladder':key+='[facing=east,waterlogged=false]'
            b.attachment_state=states[key]
        await asyncio.sleep(.2);phases['prepare']=[start,now()]
        start=now();await asyncio.sleep(warmup);phases['warmup']=[start,now()]
        start=now();await asyncio.sleep(idle);phases['idle_before']=[start,now()]
        async def work(b):
            nonlocal placed,broken,removed
            for _ in range(cycles):
                await action(b,'support',states['minecraft:stone']);placed+=1
                await action(b,'attachment',b.attachment_state);placed+=1
                await action(b,'break',0);broken+=1;removed+=1
                await asyncio.sleep(.05)
        baseline_spawns=set().union(*(b.item_spawns for b in actors))
        start=now();await asyncio.wait_for(asyncio.gather(*(work(b) for b in actors)),active_timeout)
        await until(lambda:len(set().union(*(b.item_spawns for b in actors))-baseline_spawns)>=bots*cycles,'all indirect drop entities',timeout=10)
        spawned=len(set().union(*(b.item_spawns for b in actors))-baseline_spawns);assert spawned==bots*cycles,('duplicate or extraneous drops',spawned,bots*cycles)
        phases['active']=[start,now()]
        assert placed==2*bots*cycles and broken==removed==bots*cycles
        active_sent=[{k:list(v) for k,v in r.items()} for r in sent];active_latencies=list(latencies)
        start=now();await asyncio.sleep(idle);phases['idle_after']=[start,now()]
        expected=[];start=now()
        for b in actors:
            await action(b,'support',states['minecraft:stone']);await action(b,'attachment',b.attachment_state)
            expected.extend((dict(point=list(b.target),state=states['minecraft:stone']),dict(point=list(b.attachment),state=b.attachment_state)))
        phases['durability_tail']=[start,now()]
        report=dict(phases=phases,placed=placed,broken=broken,attachments_removed=removed,observed_drop_entities=spawned,expected_drop_entities=bots*cycles,confirmed_actions=placed+broken,expected_final=expected,
                    missing=0,failures=0,acknowledged_place=placed,acknowledged_break=broken,final_validation_placements=2*bots,
                    action_times=active_sent,pickup_observations=[b.pickups for b in actors],cycle_pacing_ms=50,
                    latencies_ms=dict(p50=base.quantile(active_latencies,.5),p95=base.quantile(active_latencies,.95),p99=base.quantile(active_latencies,.99),maximum=max(active_latencies)))
        (result/'actions.json').write_text(json.dumps(report,indent=2));return report
    finally:await asyncio.gather(*(close(b) for b in actors if b.conn),return_exceptions=True)

async def run(a):
    assert a.template.is_dir() and a.repetitions>0 and a.cycles>0 and all(1<=n<=50 for n in a.bots)
    a.result_dir=a.result_dir.resolve();a.result_dir.mkdir(parents=True,exist_ok=False);a.binary=a.binary.resolve();a.item=1;a.view_distance=2
    os.environ.update(MC_QA_MODE='1',MC_ADMIN_USERNAME='StressATPR00000');base.exercise=exercise;rows=[]
    for count in a.bots:
        for rep in range(a.repetitions):
            row=await base.one(a,a.binary,'attached',count,a.result_dir/f'attached-{count}-r{rep+1}',a.template.resolve(),backend=BACKEND,save_mode='async')
            active=row['phases']['active']['storage'];received=active.get('handler_place',{}).get('calls',0)+active.get('handler_break',{}).get('calls',0)
            assert received==row['proposed_place']+row['proposed_break']==row['placed']+row['broken'],'received workload differs'
            actions=json.loads((a.result_dir/f'attached-{count}-r{rep+1}/actions.json').read_text())
            row.update(received_actions=received,repetition=rep+1,view_distance=2,observed_drop_entities=actions['observed_drop_entities']);rows.append(row)
            (a.result_dir/'comparison.json').write_text(json.dumps(rows,indent=2))
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('binary','template','result-dir'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--bots',nargs='+',type=int,default=[1,2,10,25,50]);p.add_argument('--repetitions',type=int,default=3)
    p.add_argument('--cycles',type=int,default=30);p.add_argument('--warmup',type=float,default=2);p.add_argument('--idle',type=float,default=2)
    p.add_argument('--active-timeout',type=float,default=600);p.add_argument('--fsync-probe',type=Path)
    p.add_argument('--write-trace',action='store_true');p.add_argument('--save-trace',action='store_true')
    asyncio.run(run(p.parse_args()))
