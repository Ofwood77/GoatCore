#!/usr/bin/env python3
"""Protocol 775 differential observations from two actual clients.
Expected placement states are observed on Vanilla, never synthesized by C rules.
Only disposable remote instances, no main service ports.
"""
import argparse
import asyncio
import json
from pathlib import Path
import struct
from types import SimpleNamespace
from protocol import position,read_varint,varint
from test_block_placement import hold,command,place,observed,report_states,slot_updates_since
from test_hinged_placement import HingedPlacer
from test_stair_placement import batch,world_is
from test_player_visibility import close,until
from test_player_lifecycle import latest_inventory
from test_pickup_save import collected
STEP=((0,-1,0),(0,1,0),(0,0,-1),(0,0,1),(-1,0,0),(1,0,0))
SUPPORTS=('stone','dirt','glass','oak_slab[type=bottom,waterlogged=false]',
 'oak_slab[type=top,waterlogged=false]','oak_slab[type=double,waterlogged=false]',
 'oak_stairs[facing=north,half=bottom,shape=straight,waterlogged=false]',
 'oak_stairs[facing=north,half=top,shape=straight,waterlogged=false]',
 'oak_door[facing=north,half=lower,hinge=left,open=false,powered=false]',
 'oak_trapdoor[facing=north,half=top,open=false,powered=false,waterlogged=false]',
 'oak_trapdoor[facing=north,half=bottom,open=false,powered=false,waterlogged=false]',
 'oak_trapdoor[facing=north,half=top,open=true,powered=false,waterlogged=false]',
 'oak_fence','cobblestone_wall','iron_chain[axis=y,waterlogged=false]','air')

async def run(args):
 assert args.port not in (25565,25587)
 states=report_states(args.blocks_report);names={v:k for k,v in states.items()}
 items={v['name']:v['id'] for v in json.loads(Path(args.ids).read_text())['items']}
 opts=SimpleNamespace(host='127.0.0.1',port=args.port,timeout=120,run_id='ATB',shared_zone=False,mode='connect',qa_verify_movement=False)
 item_entity=next(v['id'] for v in json.loads(Path(args.ids).read_text())['entity_types'] if v['name']=='minecraft:item')
 def spawns(who):
  count=0
  for _,payload in who.events[1]:
   _,offset=read_varint(payload);kind,_=read_varint(payload,offset+16);count+=kind==item_entity
  return count
 a,b=HingedPlacer(0,opts),HingedPlacer(1,opts);a.name='Ofwood_';C=(-16,80,16);rows=[];saved=[];loot_totals={}
 def at(p):return ' '.join(map(str,p))
 async def game(mode):
  await command(a,'gamemode '+mode)
  await until(lambda:a.gamemode=={'creative':1,'survival':0}[mode],'gamemode')
 async def teleport(point):
  nonlocal C;C=tuple(point)
  await game('creative')
  for who,x in ((a,C[0]-2.5),(b,C[0]-3.5)):
   await command(a,f'tp {who.name} {x} {C[1]} {C[2]+1.5}')
   await until(lambda:who.error or (who.pos and abs(who.pos[0]-x)<.01 and abs(who.pos[2]-C[2]-1.5)<.01),'teleport',timeout=120)
   assert not who.error,(who.name,'teleport read failure',who.error,who.pos)
   if who.reader_task.done():raise AssertionError((who.name,'reader stopped',who.reader_task.exception(),who.pos))
   await who.conn.send(30,struct.pack('>ddd?',x,C[1],C[2]+1.5,True))
   await asyncio.wait_for(asyncio.gather(*(who.chunk_ready_events[(cx,cz)].wait() for cx in range(C[0]//16-1,C[0]//16+2) for cz in range(C[2]//16-1,C[2]//16+2))),120)
  await batch(a,[f'setblock {C[0]-d} {C[1]-1} {C[2]+1} stone' for d in (3,4)])
 async def reset(support='stone',direction=0):
  await game('creative')
  commands=[f'setblock {C[0]+x} {C[1]+y} {C[2]+z} air' for x in range(-1,2) for y in range(-1,2) for z in range(-1,2)]
  p=tuple(C[i]+STEP[direction][i] for i in range(3));commands.append('setblock '+at(p)+' '+support)
  await batch(a,commands)
  return p
 async def record(tag,expected=None,network=False):
  if expected is None:
   async def get(who):
    queue=who.block_events[C];values=[]
    try:values.append(await asyncio.wait_for(queue.get(),.6))
    except asyncio.TimeoutError:pass
    while not queue.empty():values.append(queue.get_nowait())
    return values[-1] if values else states['minecraft:air']
   va,vb=await asyncio.gather(get(a),get(b));assert va==vb,(tag,va,vb)
   wanted=names[va]
  else:
   wanted=expected
   if network:await asyncio.gather(observed(a,C,states[wanted]),observed(b,C,states[wanted]))
  await world_is(a,C,wanted,states[wanted],args.vanilla,'ATTACHED_'+str(len(rows)))
  row=dict(case=tag,point=list(C),state=wanted,state_id=states[wanted]);rows.append(row)
  with Path(str(args.snapshot)+'.observations.jsonl').open('a') as f:f.write(json.dumps(row)+'\n')
  return wanted
 async def put(item='torch',face=1,yaw=0,pitch=0,clicked=None,survival=False):
  await hold(a,items['minecraft:'+item]);await a.conn.send(32,struct.pack('>ff?',float(yaw),float(pitch),True))
  # PositionMoveRotation is an observable orientation setup on BOTH servers:
  # a teleport to the same position explicitly aligns head and body yaw. An
  # action ACK alone is not a tick barrier (GoatCore can acknowledge immediately).
  corrections=a.position_corrections
  await command(a,f'tp {a.name} {a.pos[0]} {a.pos[1]} {a.pos[2]}')
  await until(lambda:a.position_corrections>corrections,'orientation sync')
  await a.conn.send(30,struct.pack('>ddd?',*a.pos[:3],True))
  if survival:await game('survival')
  for who in (a,b):who.clear_block_events(C)
  first=len(a.events[20]);click=clicked or tuple(C[i]-STEP[face][i] for i in range(3))
  await place(a,click,face,(.5,.5,.5));return first
 async def remove_support(point):
  for who in (a,b):who.clear_block_events(C)
  await batch(a,['setblock '+at(point)+' air'])
 async def break_block(point,survival=False,wait=2.0):
  for who in (a,b):who.clear_block_events(point)
  a.sequence+=1;await a.conn.send(41,varint(0)+position(*point)+varint(1)+varint(a.sequence))
  if survival:
   await asyncio.sleep(wait);a.sequence+=1
   await a.conn.send(41,varint(2)+position(*point)+varint(1)+varint(a.sequence))
  await until(lambda:any(read_varint(p)[0]==a.sequence for _,p in a.events[4]),'break ACK')
 try:
  await a.connect();await b.connect();await a.conn.send(44);await b.conn.send(44);await teleport(C)
  if args.vanilla:
   await command(a,'gamerule minecraft:random_tick_speed 0')
   if not args.verify:
    await command(a,'clear Ofwood_');await command(a,'kill @e[type=minecraft:item]')
  if args.verify:
   snap=json.loads(Path(args.snapshot).read_text())
   for i,row in enumerate(snap['saved']):
    await teleport(row['point']);await world_is(a,C,row['state'],states[row['state']],args.vanilla,'ATTACHED_RESTART_'+str(i))
   slots=latest_inventory(a)
   for item,count in snap.get('loot_totals',{}).items():assert sum(n for i,(k,n) in enumerate(slots) if i!=36 and 9<=i<45 and k==int(item))==count
   print(json.dumps(dict(status='passed',restored=len(snap['saved']))));return
  if args.ladder_traps_only:
   # Isolate placement/support from climbing physics. This target straddles
   # negative chunk and region boundaries for both X and Z support directions.
   await teleport((-512,80,512))
   last_trap=None
   for face,facing in ((2,'north'),(3,'south'),(4,'west'),(5,'east')):
    for opened in ('false','true'):
     for aligned in (True,False):
      support=await reset(direction=face^1)
      trap_facing=facing if aligned else {'north':'south','south':'north','west':'east','east':'west'}[facing]
      trap=f'minecraft:oak_trapdoor[facing={trap_facing},half=bottom,open={opened},powered=false,waterlogged=false]'
      above=(C[0],C[1]+1,C[2]);await batch(a,['setblock '+at(above)+' '+trap])
      await put('ladder',face=face);value=await record(f'ladder-trap-{facing}-{opened}-{aligned}')
      assert 'facing='+facing in value
      for who in (a,b):who.clear_block_events(C)
      count_a,count_b=spawns(a),spawns(b);await break_block(support)
      await record(f'ladder-trap-loss-{facing}-{opened}-{aligned}','minecraft:air',True)
      await world_is(a,above,trap,states[trap],args.vanilla,'TRAP_SURVIVES_'+str(len(rows)))
      await until(lambda:spawns(a)>=count_a+1 and spawns(b)>=count_b+1,'one ladder drop')
      assert spawns(a)-count_a==spawns(b)-count_b==1
      last_trap=trap
   await batch(a,['setblock '+at(support)+' stone']);await put('ladder',face=5)
   value=await record('ladder-trap-persist');saved=[dict(point=list(C),state=value),dict(point=list(above),state=last_trap)]
   for point,item,face in (((-512,90,512),'ladder',5),((-512,90,516),'lantern',0)):
    await teleport(point);await reset(direction=face^1)
    await batch(a,['setblock '+at(tuple(C[i]+STEP[d][i] for i in range(3)))+' stone' for d in range(6)])
    await batch(a,['setblock '+at(C)+' water[level=0]']);await put(item,face=face)
    wet=await record('water-persist-'+item);assert 'waterlogged=true' in wet
    saved.append(dict(point=list(C),state=wet))
   Path(args.snapshot).write_text(json.dumps(dict(saved=saved,loot_totals={}),indent=2))
   print(json.dumps(dict(status='passed',cases=rows,saved=saved,families=2,two_observers=True,trap_survival_checks=16)));return
  if not args.harvest_only:
   families=[r.split('\t') for r in Path('tests/fixtures/vanilla-26.1.1-attached.tsv').read_text().splitlines() if r.startswith('F\t')]
   # Every non-wall item: Survival consumption, observer disappearance, direct
   # Creative break without a drop. The family references came from the JAR.
   for _,full,kind,item,default in families:
    if kind=='2':continue
    block=full.split(':')[1];direction=4 if kind=='4' else 0
    p=await reset(direction=direction);start=await put(block,face=5 if kind=='4' else 1,survival=True)
    value=await record('family-'+block);assert value!='minecraft:air'
    await until(lambda:(36,int(item),3) in slot_updates_since(a,start),'Survival consumption')
    await game('creative');spawned=spawns(a);await break_block(C);await record('creative-break-'+block,'minecraft:air',True)
    await asyncio.sleep(.05);assert spawns(a)==spawned,'Creative spawned an item'
   # Creative support destruction drops the attachment, not an extra copy of
   # the ordinary support. This exercises the actual PlayerAction path.
   p=await reset();await put('torch');await record('creative-support-before')
   count_a,count_b=spawns(a),spawns(b)
   for who in (a,b):who.clear_block_events(C)
   await break_block(p);await record('creative-support-loss','minecraft:air',True)
   await until(lambda:spawns(a)>=count_a+1 and spawns(b)>=count_b+1,'one indirect Creative drop')
   assert spawns(a)-count_a==spawns(b)-count_b==1
   # Central vs full-face support, including genuinely invalid destinations.
   for item,kind in (('torch','floor'),('torch','wall'),('lantern','floor'),('lantern','hanging'),('ladder','wall')):
    d=4 if kind=='wall' else 1 if kind=='hanging' else 0
    face=d^1
    for support in SUPPORTS:
     p=await reset(support,d)
     # A replaceable clicked air cell would change the destination. Click a
     # different solid side in that one case, without changing the tested support.
     clicked=None
     if support=='air':
      clicked=(C[0],C[1],C[2]+1);await batch(a,['setblock '+at(clicked)+' stone']);face=2
     else:face=d^1
     await put(item,face=face,clicked=clicked);value=await record(f'support-{item}-{kind}-{support}')
     if value!='minecraft:air':
      # Remove the actual support selected by Vanilla, not a guessed candidate.
      if 'wall_torch' in value or 'ladder' in value:
       facing=dict(p.split('=') for p in value.split('[')[1][:-1].split(','))['facing'];sd={'north':3,'south':2,'west':5,'east':4}[facing]
      else:sd=1 if 'hanging=true' in value else 0
      p=tuple(C[i]+STEP[sd][i] for i in range(3))
      await remove_support(p);await record(f'loss-{item}-{kind}-{support}','minecraft:air',True)
   # Real directional look ordering, all faces, alternatives, and both vertical
   # supports present; do not hardcode which candidate Vanilla selects.
   for item in ('torch','soul_torch','copper_torch','lantern','ladder'):
    for face in range(6):
     d=face^1;await reset(direction=d);await put(item,face=face,yaw=90*face,pitch=-45 if face&1 else 45)
     await record(f'faces-{item}-{face}')
    for yaw,pitch in ((-135,-45),(-45,0),(45,45),(135,-45),(0,90),(0,-90)):
     await reset(direction=0);await batch(a,['setblock '+at(tuple(C[i]+STEP[d][i] for i in range(3)))+' stone' for d in (1,2,3,4,5)])
     await put(item,face=1,yaw=yaw,pitch=pitch,clicked=C);await record(f'alternatives-{item}-{yaw}-{pitch}')
   # The support changes indirectly, not adjacent to the wall torch: an
   # inner stair loses its full side after its southern neighbor disappears.
   await reset();support=(C[0]+1,C[1],C[2]);back=(C[0]+1,C[1],C[2]+1)
   await batch(a,['setblock '+at(support)+' oak_stairs[facing=north,half=bottom,shape=straight,waterlogged=false]',
                  'setblock '+at(back)+' oak_stairs[facing=west,half=bottom,shape=straight,waterlogged=false]'])
   await put('torch',face=4,clicked=support);await record('stair-derived-support-before')
   await remove_support(back);await record('stair-derived-support-lost','minecraft:air',True)
   # Ladder front-extension refusal and the distinction from climbing/trapdoors.
   for facing in ('north','south','east','west'):
    d={'north':2,'south':3,'west':4,'east':5}[facing]
    await reset(direction=d^1);p=tuple(C[i]+STEP[d^1][i] for i in range(3))
    await batch(a,['setblock '+at(p)+f' ladder[facing={facing},waterlogged=false]'])
    await put('ladder',face=d,clicked=p);await record('ladder-front-'+facing)
   # Actual entity obstruction, independently of the extracted shape table.
   for item,face in (('torch',1),('lantern',1),('ladder',5)):
    for who,label in ((a,'self'),(b,'observer')):
     await reset(direction=face^1)
     x=C[0]+(.1 if item=='ladder' else .5);z=C[2]+.5
     await command(a,f'tp {who.name} {x} {C[1]} {z}')
     await until(lambda:who.pos and abs(who.pos[0]-x)<.01 and abs(who.pos[2]-z)<.01,'obstruction teleport')
     await who.conn.send(30,struct.pack('>ddd?',x,C[1],z,True))
     await put(item,face=face);value=await record('obstruction-'+item+'-'+label)
     assert (value!='minecraft:air')==(item=='torch')
     await teleport(C)
   # Sealed source water: source-state waterlogging and direct destruction
   # restoring source water. No flowing-water simulation claim.
   for item,face in (('lantern',1),('ladder',5)):
    await reset(direction=face^1);await batch(a,['setblock '+at(tuple(C[i]+STEP[d][i] for i in range(3)))+' stone' for d in range(6) if d!=(face^1)])
    await batch(a,['setblock '+at(C)+' water[level=0]']);await put(item,face=face)
    wet=await record('water-'+item);assert 'waterlogged=true' in wet
    await game('creative');await break_block(C);await record('water-break-'+item,'minecraft:water[level=0]',True)
    await put(item,face=face);await record('water-replace-'+item)
    support=tuple(C[i]+STEP[face^1][i] for i in range(3))
    await remove_support(support);await record('water-support-loss-'+item,'minecraft:water[level=0]',True)
   # One support update must affect all attached neighbors exactly once, and
   # the top trapdoor exception stays independent of this support system.
   await reset();await put('torch');await record('support-trap-before')
   await batch(a,['setblock '+at((C[0],C[1]-1,C[2]))+' oak_trapdoor[facing=north,half=top,open=false,powered=false,waterlogged=false]'])
   for who in (a,b):who.clear_block_events(C)
   await place(a,(C[0],C[1]-1,C[2]),1);await record('support-trap-open','minecraft:air',True)
   # Six dependents of one support: two observers receive every removal and
   # exactly six item entities, even though removals themselves update neighbors.
   center=C;await reset('air');await batch(a,['setblock '+at(center)+' stone'])
   dependents=[]
   for d,item in enumerate(('lantern','torch','soul_torch','copper_torch','torch','torch')):
    C=tuple(center[i]+STEP[d][i] for i in range(3));await put(item,face=d,clicked=center)
    value=await record('shared-support-place-'+str(d));assert value!='minecraft:air';dependents.append(C)
   for who in (a,b):
    for p in dependents:who.clear_block_events(p)
   count_a,count_b=spawns(a),spawns(b);await batch(a,['setblock '+at(center)+' air'])
   for d,p in enumerate(dependents):C=p;await record('shared-support-loss-'+str(d),'minecraft:air',True)
   await until(lambda:spawns(a)>=count_a+6 and spawns(b)>=count_b+6,'six item entities')
   assert spawns(a)-count_a==spawns(b)-count_b==6;C=center
   # Earlier geometry/support scenarios may leave collectible items. Start
   # inventory accounting at a defined boundary, through the same Creative
   # slot protocol on both servers, before entering the private pickup lanes.
   await game('creative')
   for slot in range(9,46):await a.conn.send(56,struct.pack('>h',slot)+varint(0))
   await asyncio.sleep(.2)
   # Indirect drops, pickup observed by both, inventory verified after reconnect.
   for index,(item,face) in enumerate((('torch',1),('soul_torch',5),('copper_torch',5),('lantern',1),('soul_lantern',0),('ladder',5))):
    await teleport((-16,80,48+index*4));p=await reset(direction=face^1);await put(item,face=face);await record('drop-place-'+item)
    # Catch drops before removing support. Otherwise a fast, unstalled tick
    # lets the item fall below the later floor while the state query completes.
    # This fixture isolates loot/persistence from unqualified falling physics.
    await batch(a,['setblock '+at((C[0],C[1]-2,C[2]))+' stone'])
    await hold(a,items['minecraft:stone']);before_a,before_b=collected(a),collected(b)
    await remove_support(p);await record('drop-loss-'+item,'minecraft:air',True)
    await batch(a,['setblock '+at((C[0],C[1]-1,C[2]))+' stone']);await command(a,f'tp {C[0]+.5} {C[1]} {C[2]+.5}');await a.conn.send(30,struct.pack('>ddd?',C[0]+.5,C[1],C[2]+.5,True))
    await until(lambda:collected(a)>before_a and collected(b)>before_b,'drop collected',timeout=12)
    assert collected(a)-before_a==collected(b)-before_b==1
    loot_totals[items['minecraft:'+item]]=loot_totals.get(items['minecraft:'+item],0)+1
    rows[-1].update(drop_item=items['minecraft:'+item],drop_count=1)
  # Direct Survival harvesting: correct non-stackable tools, no double drop.
  for index,item in enumerate(('torch','soul_torch','copper_torch','ladder','lantern','soul_lantern','waxed_oxidized_copper_lantern')):
   await teleport((-16,80,84+index*4));face=5 if item=='ladder' else 1
   await reset(direction=face^1);await batch(a,[f'setblock {C[0]+dx} {C[1]-1} {C[2]+dz} stone' for dx in range(-1,2) for dz in range(-1,2)])
   await put(item,face=face);await record('direct-place-'+item)
   tool='diamond_pickaxe' if 'lantern' in item else 'diamond_axe'
   await a.conn.send(56,struct.pack('>h',36)+varint(1)+varint(items['minecraft:'+tool])+b'\0\0');await a.conn.send(53,struct.pack('>h',0))
   if args.vanilla:
    first=len(a.events[121])
    for attempt in range(10):
     await command(a,'data get entity Ofwood_ SelectedItem.id')
     if any(('minecraft:'+tool).encode() in p for _,p in a.events[121][first:]):break
    else:raise AssertionError('selected tool not applied')
   else:await until(lambda:(36,items['minecraft:'+tool],1) in slot_updates_since(a,0),'tool slot acknowledged')
   await game('survival');await a.conn.send(32,struct.pack('>ff?',0.,0.,True));await a.conn.send(30,struct.pack('>ddd?',*a.pos[:3],True))
   before_a,before_b=collected(a),collected(b)
   await break_block(C,survival=True);await record('direct-break-'+item,'minecraft:air',True)
   await command(a,f'tp {C[0]+.5} {C[1]} {C[2]+.5}');await a.conn.send(30,struct.pack('>ddd?',C[0]+.5,C[1],C[2]+.5,True))
   await until(lambda:collected(a)>before_a and collected(b)>before_b,'direct drop collected',timeout=12)
   assert collected(a)-before_a==collected(b)-before_b==1
   loot_totals[items['minecraft:'+item]]=loot_totals.get(items['minecraft:'+item],0)+1
   rows[-1].update(drop_item=items['minecraft:'+item],drop_count=1)
  # Official 26.1.1 lantern properties do not require a correct tool for
  # drops. Verify this with stone in hand, separately from pickaxe speed.
  await teleport((-16,80,116));await reset();await batch(a,[f'setblock {C[0]+dx} {C[1]-1} {C[2]+dz} stone' for dx in range(-1,2) for dz in range(-1,2)])
  await put('lantern');await record('wrong-tool-lantern-place');await hold(a,items['minecraft:stone']);await game('survival')
  await a.conn.send(30,struct.pack('>ddd?',*a.pos[:3],True));before_a,before_b=collected(a),collected(b)
  await break_block(C,survival=True,wait=6.0);await record('wrong-tool-lantern-break','minecraft:air',True)
  await command(a,f'tp {C[0]+.5} {C[1]} {C[2]+.5}');await a.conn.send(30,struct.pack('>ddd?',C[0]+.5,C[1],C[2]+.5,True))
  await until(lambda:collected(a)>before_a and collected(b)>before_b,'wrong-tool lantern pickup',timeout=12)
  assert collected(a)-before_a==collected(b)-before_b==1
  loot_totals[items['minecraft:lantern']]=loot_totals.get(items['minecraft:lantern'],0)+1
  rows[-1].update(drop_item=items['minecraft:lantern'],drop_count=1)
  # A refused Survival action must preserve all four held items on reconnect.
  await teleport((-16,80,128));await reset('air')
  await put('torch',clicked=C,survival=True);await record('deny-no-support','minecraft:air')
  await close(a);a=HingedPlacer(0,opts);a.name='Ofwood_';await a.connect();await a.conn.send(44)
  await until(lambda:bool(a.events[18]),'rejected inventory reconnect');assert latest_inventory(a)[36]==(items['minecraft:torch'],4)
  # Separate, widely spaced sentinels so preparing one cannot erase another.
  for point,item,face in [((-513,80,511),'copper_torch',5),((-512,80,516),'waxed_oxidized_copper_lantern',0),((511,80,-513),'ladder',5),((512,80,-508),'soul_lantern',1)]:
   await teleport(point);await reset(direction=face^1);await put(item,face=face,survival=True);value=await record('persist-'+item)
   assert value!='minecraft:air';saved.append(dict(point=list(C),state=value))
  await close(a);a=HingedPlacer(0,opts);a.name='Ofwood_';await a.connect();await a.conn.send(44)
  await until(lambda:bool(a.events[18]),'reconnect inventory');slots=latest_inventory(a)
  for item,count in loot_totals.items():assert sum(n for i,(k,n) in enumerate(slots) if i!=36 and 9<=i<45 and k==item)==count,(item,count,slots)
  Path(args.snapshot).write_text(json.dumps(dict(saved=saved,loot_totals=loot_totals),indent=2))
  print(json.dumps(dict(status='passed',cases=rows,saved=saved,families=0 if args.harvest_only else 17,two_observers=True)))
 finally:await asyncio.gather(*(close(p) for p in (a,b) if p.conn and not p.conn.writer.is_closing()),return_exceptions=True)
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',type=int,required=True);p.add_argument('--snapshot',required=True)
 p.add_argument('--blocks-report',default='data/26.1.1/reports/blocks.json');p.add_argument('--ids',default='src/generated/generated_minecraft_ids.json')
 p.add_argument('--ladder-traps-only',action='store_true',help='supplemental support/placement corpus; no climbing claim')
 p.add_argument('--harvest-only',action='store_true',help='targeted tool/drop pilot')
 p.add_argument('--verify',action='store_true');p.add_argument('--vanilla',action='store_true');asyncio.run(run(p.parse_args()))
