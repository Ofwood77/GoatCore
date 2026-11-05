#!/usr/bin/env python3
"""Fixed-work hinged block load, reusing the continuous tick/IO harness.

Remote isolated instances only. Every cycle confirms placement, opening,
closing and destruction with a state packet AND the matching sequence ACK.
Shutdown, independent disk decoding and a fresh server restart are included.
"""
import argparse
import asyncio
import json
import math
import os
from pathlib import Path
import struct
import time
import profile_tick as base
from protocol import position,varint
from stress import options,block_target_offset
from test_player_visibility import close
from test_block_placement import report_states

async def exercise(port,bots,cycles,item,result,warmup,idle,prepare_only,active_timeout=300):
    opts=options(['--host','127.0.0.1','--port',str(port),'--mode','blocks','--bots',str(bots),
                  '--allow-block-changes','--timeout','60'])
    opts.run_id='PROF';opts.place_limiter=None;opts.break_limiter=None
    actors=[base.ProfileBot(i,opts,lambda _:None) for i in range(bots)]
    states=report_states(base.ROOT/'data/26.1.1/reports/blocks.json');names={v:k for k,v in states.items()}
    ids=json.loads((base.ROOT/'src/generated/generated_minecraft_ids.json').read_text())
    items={v['name']:v['id'] for v in ids['items']}
    now=lambda:time.monotonic_ns()//1000
    phases={};sent=[dict(place_sent=[],break_sent=[],interaction_sent=[]) for _ in actors]
    placed=broken=interactions=0;action_latencies=[]
    async def connect(bot):
        await bot.connect()
        deadline=time.monotonic()+120
        while sum(e.is_set() for e in bot.chunk_ready_events.values())<441:
            if time.monotonic()>deadline:raise TimeoutError('441 initial chunks')
            await asyncio.sleep(.05)
        opts_id=items['minecraft:oak_door' if bot.index%2==0 else 'minecraft:oak_trapdoor']
        await bot.conn.send(56,struct.pack('>h',36)+varint(64)+varint(opts_id)+b'\0\0')
        await bot.conn.send(53,struct.pack('>h',0));await asyncio.sleep(.1)
        x,z=block_target_offset(bot.index);bot.target=(math.floor(bot.pos[0])+x,64,math.floor(bot.pos[2])+z)
    async def action(bot,kind,expected=None):
        started=time.monotonic()
        point=bot.target;x,y,z=point;door=bot.index%2==0
        points=[point,(x,y+1,z)] if door else [point]
        for p in points:bot.clear_block_events(p)
        bot.sequence+=1
        sent[bot.index][{'place':'place_sent','break':'break_sent','use':'interaction_sent'}[kind]].append(now())
        if kind=='break':payload=varint(0)+position(*point)+varint(1)+varint(bot.sequence);packet=41
        else:
            clicked=(x,y-1,z) if kind=='place' else point
            payload=varint(0)+position(*clicked)+varint(1)+struct.pack('>fff??',.5,1.,.5,False,False)+varint(bot.sequence);packet=66
        await bot.conn.send(packet,payload)
        value=await bot.expect_block(point,(lambda s:s!=0) if expected is None else (lambda s:s==expected))
        assert value is not None,(bot.index,kind,'missing state/ACK')
        if door:
            upper=0 if value==0 else states[names[value].replace('half=lower','half=upper')]
            assert await bot.expect_block(points[1],lambda s:s==upper) is not None,(bot.index,kind,'upper')
        action_latencies.append((time.monotonic()-started)*1000)
        return value
    try:
        start=now()
        for bot in actors:
            await connect(bot)
        phases['prepare']=[start,now()]
        start=now();await asyncio.sleep(warmup);phases['warmup']=[start,now()]
        start=now();await asyncio.sleep(idle);phases['idle_before']=[start,now()]
        for b in actors:b.latencies.clear();b.acknowledged.clear()
        async def work(bot):
            nonlocal placed,broken,interactions
            for _ in range(cycles):
                initial=await action(bot,'place');placed+=1
                opened=states[names[initial].replace('open=false','open=true')]
                await action(bot,'use',opened);interactions+=1
                await action(bot,'use',initial);interactions+=1
                await action(bot,'break',0);broken+=1
                await asyncio.sleep(.05)
        start=now()
        if not prepare_only:await asyncio.wait_for(asyncio.gather(*(work(b) for b in actors)),active_timeout)
        phases['active']=[start,now()]
        if not prepare_only:assert placed==broken==bots*cycles and interactions==2*bots*cycles
        start=now();await asyncio.sleep(idle);phases['idle_after']=[start,now()]
        latencies=list(action_latencies)
        active_sent=[{k:list(times) for k,times in v.items()} for v in sent]
        expected=[];start=now()
        if not prepare_only:
            for b in actors:
                closed=await action(b,'place');opened=states[names[closed].replace('open=false','open=true')]
                await action(b,'use',opened);expected.append(dict(point=list(b.target),state=opened))
                if b.index%2==0:
                    x,y,z=b.target;expected.append(dict(point=[x,y+1,z],state=states[names[opened].replace('half=lower','half=upper')]))
        phases['durability_tail']=[start,now()]
        report=dict(phases=phases,placed=placed,broken=broken,interactions=interactions,confirmed_actions=placed+broken+interactions,cycle_pacing_ms=50,
                    expected_final=expected,final_validation_placements=0 if prepare_only else bots,missing=0,failures=0,
                    acknowledged_place=placed,acknowledged_break=broken,action_times=active_sent,
                    pickup_observations=[],latencies_ms=dict(p50=base.quantile(latencies,.5),p95=base.quantile(latencies,.95),p99=base.quantile(latencies,.99),maximum=max(latencies,default=None)))
        (result/'actions.json').write_text(json.dumps(report,indent=2));return report
    finally:await asyncio.gather(*(close(b) for b in actors if b.conn),return_exceptions=True)

async def run(a):
    a.result_dir=a.result_dir.resolve();a.result_dir.mkdir(parents=True,exist_ok=False)
    a.binary=a.binary.resolve();a.item=1
    os.environ.update(MC_QA_MODE='1',MC_ADMIN_USERNAME='StressPROF00000')
    base.exercise=exercise
    template=a.template.resolve() if a.template else None
    if not template:
        # Use the original preparation helper, with a stone item, for equivalent
        # preloaded empty targets; no door is left in the measured template.
        base.exercise=ORIGINAL
        await base.one(a,a.binary,'template',1,a.result_dir/'template',prepare_only=True,backend=BACKEND,save_mode='async')
        template=a.result_dir/'template/runtime/world';base.exercise=exercise
    rows=[]
    for count in a.bots:
        for rep in range(a.repetitions):
            directory=a.result_dir/f'hinged-{count}-r{rep+1}'
            row=await base.one(a,a.binary,'hinged',count,directory,template,backend=BACKEND,save_mode='async')
            actions=json.loads((directory/'actions.json').read_text())
            received_place=row['phases']['active']['storage'].get('handler_place',{}).get('calls',0)
            received_break=row['phases']['active']['storage'].get('handler_break',{}).get('calls',0)
            assert received_place==3*actions['placed'] and received_break==actions['broken'],'received workload differs'
            row.update(received_actions=received_place+received_break,interactions=actions['interactions'],confirmed_actions=actions['confirmed_actions'],proposed_interactions=sum(len(v['interaction_sent']) for v in actions['action_times']),repetition=rep+1)
            rows.append(row);(a.result_dir/'comparison.json').write_text(json.dumps(rows,indent=2))
BACKEND='goatworld-sqlite-experimental';ORIGINAL=base.exercise
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary',type=Path,required=True);p.add_argument('--template',type=Path)
    p.add_argument('--result-dir',type=Path,required=True);p.add_argument('--bots',nargs='+',type=int,default=[1,2,10,25,50])
    p.add_argument('--repetitions',type=int,default=3);p.add_argument('--cycles',type=int,default=200)
    p.add_argument('--warmup',type=float,default=2);p.add_argument('--idle',type=float,default=2)
    p.add_argument('--active-timeout',type=float,default=600);p.add_argument('--fsync-probe',type=Path)
    p.add_argument('--write-trace',action='store_true');p.add_argument('--save-trace',action='store_true')
    asyncio.run(run(p.parse_args()))
