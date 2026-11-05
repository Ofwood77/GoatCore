#!/usr/bin/env python3
"""Same real protocol-775 actions on disposable Vanilla and GoatCore worlds.

Successful states come from BOTH clients, followed by an authoritative query.
No GoatCore hinge algorithm is used to manufacture a Vanilla reference.
"""
import argparse
import asyncio
import json
from pathlib import Path
import struct
from types import SimpleNamespace
from protocol import position,read_varint,varint
from test_block_placement import Placer,hold,command,place,observed,report_states,slot_updates_since
from test_stair_placement import batch,world_is
from test_player_visibility import close,until
from test_player_lifecycle import latest_inventory
from test_pickup_save import collected

DIR=['north','east','south','west'];DX=[0,1,0,-1];DZ=[-1,0,1,0]
class HingedPlacer(Placer):
 async def play_packet(self,packet_id,payload):
  if packet_id==38 and payload[0]==3:self.gamemode=int(struct.unpack_from('>f',payload,1)[0])
  if packet_id in (117,124):self.events[packet_id].append((asyncio.get_running_loop().time(),payload))
  await super().play_packet(packet_id,payload)

async def run(args):
 assert args.port not in (25565,25587)
 states=report_states(args.blocks_report);names={v:k for k,v in states.items()}
 items={i['name']:i['id'] for i in json.loads(Path(args.ids).read_text())['items']}
 item_entity=next(i['id'] for i in json.loads(Path(args.ids).read_text())['entity_types'] if i['name']=='minecraft:item')
 def item_spawns(who):
  count=0
  for _,payload in who.events[1]:
   _,offset=read_varint(payload);kind,_=read_varint(payload,offset+16);count+=kind==item_entity
  return count
 families=[line.split('\t')[1:4] for line in Path('tests/fixtures/vanilla-26.1.1-hinged-shapes.tsv').read_text().splitlines() if line.startswith('F\t')]
 opts=SimpleNamespace(host='127.0.0.1',port=args.port,timeout=30,run_id='HINGE',shared_zone=False,mode='connect',qa_verify_movement=False)
 a,b=HingedPlacer(0,opts),HingedPlacer(1,opts);a.name='Ofwood_';rows=[];saved=[];C=(-16,64,16)
 sound_ids={r[1]:(int(r[5]),int(r[6])) for r in (line.split('\t') for line in Path('tests/fixtures/vanilla-26.1.1-hinged-shapes.tsv').read_text().splitlines()) if r[0]=='F'}
 if args.core_materials:families=[f for f in families if f[0] in {'minecraft:'+m+'_'+kind for m in ('oak','iron','copper') for kind in ('door','trapdoor')}]
 loot_totals={}
 def key(block,**props):return 'minecraft:'+block+'['+','.join(f'{k}={v}' for k,v in sorted(props.items()))+']'
 def door(f=2,hinge='left',opened='false',half='lower',block='oak_door'):
  return key(block,facing=DIR[f],half=half,hinge=hinge,open=opened,powered='false')
 async def game(mode,actor=None):
  actor=actor or a
  await command(a,f'gamemode {mode} {actor.name}')
  await until(lambda:actor.gamemode=={'creative':1,'survival':0}[mode],'gamemode applied')
 async def teleport(point=C,actor=None):
  nonlocal C
  C=tuple(point)
  for who,x in ((a,C[0]-2.5),(b,C[0]-3.5)):
   await command(a,f'tp {who.name} {x} {C[1]} {C[2]+1.5}')
   await until(lambda:who.pos and abs(who.pos[0]-x)<.01 and abs(who.pos[2]-(C[2]+1.5))<.01,'teleport echo')
   await who.conn.send(30,struct.pack('>ddd?',x,C[1],C[2]+1.5,True))
   await asyncio.wait_for(who.chunk_ready_events[(C[0]//16,C[2]//16)].wait(),30)
  await batch(a,[f'setblock {C[0]-d} {C[1]-1} {C[2]+1} stone' for d in (3,4)])
  await a.conn.send(32,struct.pack('>ff?',0.,0.,True))
 async def clean(radius=1):
  await game('creative')
  await batch(a,[f'setblock {C[0]} {C[1]} {C[2]} air',f'setblock {C[0]} {C[1]+1} {C[2]} air']+
   [f'setblock {C[0]+x} {C[1]+y} {C[2]+z} '+('stone' if y==-1 else 'air')
    for x in range(-radius,radius+1) for z in range(-radius,radius+1) for y in (-1,0,1)])
 async def clear_events(points):
  for who in (a,b):
   for point in points:who.clear_block_events(point)
 async def record(tag,points,block=None,expected=None,network_expected=False):
  result=[]
  for i,p in enumerate(points):
   if expected is None:
    async def next_state(who):
     while True:
      value=await asyncio.wait_for(who.block_events[p].get(),8)
      if names[value].split('[')[0]=='minecraft:'+block:return value
    va,vb=await asyncio.gather(next_state(a),next_state(b));assert va==vb,(tag,va,vb)
    wanted=names[va]
   else:
    wanted=expected[i]
    if network_expected:await asyncio.gather(observed(a,p,states[wanted]),observed(b,p,states[wanted]))
   await world_is(a,p,wanted,states[wanted],args.vanilla,'HINGED_'+str(len(rows))+'_'+str(i))
   result.append(dict(point=list(p),state=wanted,state_id=states[wanted]))
  rows.append(dict(case=tag,states=result))
  with Path(str(args.snapshot)+'.observations.jsonl').open('a') as log:log.write(json.dumps(rows[-1])+'\n')
  return [x['state'] for x in result]
 async def put(block='oak_door',yaw=0,face=1,hit=(.5,1.,.5),survival=False,clicked=None,accept=True):
  await hold(a,items['minecraft:'+block]);await a.conn.send(32,struct.pack('>ff?',float(yaw),0.,True))
  if survival:await game('survival')
  await clear_events([C,(C[0],C[1]+1,C[2])]);start=len(a.events[20])
  await place(a,clicked or (C[0],C[1]-1,C[2]),face,hit)
  if survival and accept:await until(lambda:(36,items['minecraft:'+block],3) in slot_updates_since(a,start),'placement consumption 4 to 3')
 async def use(point,actor=None):
  actor=actor or a
  # Empty main hand avoids unrelated item-on-block mechanics.
  await game('creative',actor)
  await actor.conn.send(56,struct.pack('>h',36)+varint(0));await actor.conn.send(53,struct.pack('>h',0))
  await clear_events([C,(C[0],C[1]+1,C[2])]);await place(actor,point,1,(.5,.5,.5))
 async def destroy(point,survival=False):
  await clear_events([C,(C[0],C[1]+1,C[2])]);a.sequence+=1
  await a.conn.send(41,varint(0)+position(*point)+varint(1)+varint(a.sequence))
  if survival:
   await asyncio.sleep(6.0);a.sequence+=1
   await a.conn.send(41,varint(2)+position(*point)+varint(1)+varint(a.sequence))
  await until(lambda:any(read_varint(p)[0]==a.sequence for _,p in a.events[4]),'destroy ACK')
 try:
  await a.connect();await b.connect();await a.conn.send(44);await b.conn.send(44)
  await teleport()
  if args.vanilla:await command(a,'gamerule minecraft:random_tick_speed 0')
  if args.verify:
   snap=json.loads(Path(args.snapshot).read_text())
   for i,entry in enumerate(snap['saved']):
    await teleport(entry['point']);await world_is(a,tuple(entry['point']),entry['state'],states[entry['state']],args.vanilla,'HINGE_RESTART_'+str(i))
   slots=latest_inventory(a)
   for item,count in snap.get('loot_totals',{}).items():assert sum(n for i,(item_id,n) in enumerate(slots) if i!=36 and 9<=i<45 and item_id==int(item))==count
   print(json.dumps(dict(status='passed',restored=len(snap['saved']))));return
  # Every official material: real placement, both halves, two hand toggles,
  # second observer closes independently; copper/iron are not guessed by name.
  for full,kind,manual in families:
   block=full.split(':')[1];door_family=kind=='1';points=[C,(C[0],C[1]+1,C[2])] if door_family else [C]
   await clean();await put(block,survival=True)
   placed=await record('family-place-'+block,points,block=block)
   for turn,actor in enumerate((a,b)):
    receiver=b if actor is a else a;before_sound=len(receiver.events[117])
    await use(points[-1] if turn==0 else points[0],actor)
    await record('family-use-'+block+'-'+str(turn),points,block=block,expected=placed if manual=='0' else None)
    if manual=='1':
     await until(lambda:len(receiver.events[117])>before_sound,'observer open/close sound')
     payload=receiver.events[117][-1][1];sound,at=read_varint(payload);source,at=read_varint(payload,at)
     pos=struct.unpack_from('>iii',payload,at);volume,pitch=struct.unpack_from('>ff',payload,at+12)
     assert sound-1==sound_ids[full][turn] and source==4 and volume==1 and .8999<=pitch<=1.0001 and at+28==len(payload)
     rows[-1]['observer_sound']=dict(id=sound-1,source=source,position=list(pos),volume=volume,pitch_in_range=True)
    else:assert len(receiver.events[117])==before_sound
   await game('creative');spawned=item_spawns(a);await destroy(points[-1])
   await record('creative-destroy-'+block,points,expected=['minecraft:air']*len(points),network_expected=True)
   await asyncio.sleep(.05);assert item_spawns(a)==spawned,'Creative spawned a drop'
  # Direction boundaries, exact hit edge tie and obstacle voting. Neighbour
  # doors are intentionally different materials and may already be open.
  for f in range(4):
   yaw=((f+2)&3)*90
   for edge in (.25,.5,.75):
    await clean();await put(yaw=yaw,hit=(edge,1.,edge))
    await record(f'hinge-hit-{f}-{edge}',[C,(C[0],C[1]+1,C[2])],block='oak_door')
    if edge!=.5:
     for turn in range(2):await use(C);await record(f'hinge-use-{f}-{edge}-{turn}',[C,(C[0],C[1]+1,C[2])],block='oak_door')
   for side in (-1,1):
    d=(f+side)&3
    for obstacle in ('stone','oak_slab[type=top,waterlogged=false]','door-closed','door-open'):
     await clean();x,z=C[0]+DX[d],C[2]+DZ[d]
     if obstacle.startswith('door'):
      opened='true' if obstacle=='door-open' else 'false'
      await batch(a,[f'setblock {x} {C[1]} {z} '+door(f,opened=opened,block='spruce_door'),f'setblock {x} {C[1]+1} {z} '+door(f,half='upper',opened=opened,block='spruce_door')])
     else:await batch(a,[f'setblock {x} {C[1]+h} {z} {obstacle}' for h in (0,1)])
     await put(yaw=yaw);await record(f'hinge-neighbor-{f}-{side}-{obstacle}',[C,(C[0],C[1]+1,C[2])],block='oak_door')
  for yaw in (-135.01,-135.,-134.99,-45.01,-45.,-44.99,44.99,45.,45.01,134.99,135.,135.01):
   await clean();await put(yaw=yaw);await record('yaw-'+str(yaw),[C],block='oak_door')
  # Door floor support versus trapdoor's deliberate lack of support requirement.
  for support in ('air','stone','oak_slab[type=bottom,waterlogged=false]','oak_slab[type=top,waterlogged=false]',
                  'oak_stairs[facing=north,half=bottom,shape=straight,waterlogged=false]',
                  'oak_stairs[facing=north,half=top,shape=straight,waterlogged=false]'):
   await clean();await batch(a,[f'setblock {C[0]} {C[1]-1} {C[2]} {support}'])
   accepted=support not in ('air','oak_slab[type=bottom,waterlogged=false]','oak_stairs[facing=north,half=bottom,shape=straight,waterlogged=false]')
   clicked=(C[0],C[1],C[2]+1);await batch(a,[f'setblock {" ".join(map(str,clicked))} stone'])
   await put(face=2,hit=(.5,.25,.5),clicked=clicked);await record('door-support-'+support,[C,(C[0],C[1]+1,C[2])],block='oak_door',expected=None if accepted else ['minecraft:air']*2)
  # Side placements and replacing the clicked replaceable block have different
  # orientation/half rules in TrapDoorBlock.getStateForPlacement.
  for face in range(6):
   for hit_y in (.25,.5,.75):
    await clean();offset=((0,-1,0),(0,1,0),(0,0,-1),(0,0,1),(-1,0,0),(1,0,0))[face]
    clicked=tuple(C[i]-offset[i] for i in range(3));await batch(a,[f'setblock {" ".join(map(str,clicked))} stone'])
    await put('oak_trapdoor',face=face,hit=(.5,hit_y,.5),clicked=clicked)
    await record(f'trap-face-{face}-{hit_y}',[C],block='oak_trapdoor')
    if face>=2 and hit_y!=.5:
     for turn in range(2):await use(C);await record(f'trap-face-use-{face}-{hit_y}-{turn}',[C],block='oak_trapdoor')
  for face in (1,2,3,4,5):
   await clean();await batch(a,[f'setblock {" ".join(map(str,C))} short_grass'])
   await put('oak_trapdoor',face=face,hit=(.5,.75,.5),clicked=C)
   await record(f'trap-replace-{face}',[C],block='oak_trapdoor')
  await clean();await put('oak_trapdoor');old=await record('trap-supported',[C],block='oak_trapdoor')
  await batch(a,[f'setblock {C[0]} {C[1]-1} {C[2]} air']);await record('trap-support-removed',[C],expected=old)
  # Occupied upper cell, invalid support and actual thin-shape collisions.
  await clean();await batch(a,[f'setblock {C[0]} {C[1]+1} {C[2]} stone']);await put(survival=True,accept=False)
  await record('deny-upper-occupied',[C,(C[0],C[1]+1,C[2])],expected=['minecraft:air','minecraft:stone'])
  await close(a);a=HingedPlacer(0,opts);a.name='Ofwood_';await a.connect();await a.conn.send(44)
  await until(lambda:bool(a.events[18]),'rejected placement inventory');assert latest_inventory(a)[36]==(items['minecraft:oak_door'],4)
  for who,label in ((a,'self'),(b,'observer')):
   await clean();await command(a,f'tp {who.name} {C[0]+.5} {C[1]} {C[2]+.1}')
   await until(lambda:who.pos and abs(who.pos[0]-(C[0]+.5))<.01,'collision teleport')
   await put();await record('deny-'+label+'-collision',[C,(C[0],C[1]+1,C[2])],expected=['minecraft:air']*2)
   await teleport()
  # BlockItem checks only the lower state for entity obstruction. An upper-half
  # overlap is not an extra placement veto invented by GoatCore.
  await clean();await command(a,f'tp {C[0]+.5} {C[1]+1} {C[2]+.1}')
  await put();await record('upper-body-overlap',[C,(C[0],C[1]+1,C[2])],block='oak_door');await teleport()
  for high,ok in ((318,True),(319,False)):
   await teleport((-16,high,16));await clean();await put()
   await record('world-top-'+str(high),[C],block='oak_door',expected=None if ok else ['minecraft:air'])
  await teleport((-16,64,16));await clean();await put()
  await hold(a,items['minecraft:stone']);await game('survival');await clear_events([C,(C[0],C[1]+1,C[2])]);start=len(a.events[20])
  await place(a,C,1,(.5,.5,.5));await record('survival-use-holding-block',[C,(C[0],C[1]+1,C[2])],block='oak_door')
  # An unchanged slot need not be resent by Vanilla. Reconnect for the full
  # authoritative inventory rather than requiring a redundant packet.
  await close(a);a=HingedPlacer(0,opts);a.name='Ofwood_';await a.connect();await a.conn.send(44)
  await until(lambda:bool(a.events[18]),'interaction inventory');assert latest_inventory(a)[36]==(items['minecraft:stone'],4)
  # Harvest actual items with the correct tool, separately for each door half.
  # Pickup packet counts are checked on both observers; inventory is checked on
  # reconnection, without resetting the slots receiving these drops.
  drop_cases=[('oak_door',0),('oak_door',1),('iron_door',0),('iron_door',1),('copper_door',0),('copper_door',1),
              ('oak_trapdoor',0),('iron_trapdoor',0),('copper_trapdoor',0)]
  for index,(block,half) in enumerate(drop_cases):
   await teleport((-16,64,48+index*4));await clean();await put(block)
   points=[C,(C[0],C[1]+1,C[2])] if block.endswith('_door') and not block.endswith('_trapdoor') else [C]
   await record('drop-placed-'+block+'-'+str(half),points,block=block)
   tool='diamond_pickaxe' if 'iron' in block or 'copper' in block else 'diamond_axe'
   await game('creative');await a.conn.send(56,struct.pack('>h',36)+varint(1)+varint(items['minecraft:'+tool])+b'\0\0')
   await a.conn.send(53,struct.pack('>h',0))
   if args.vanilla:
    # Query actual SelectedItem NBT, not a slot-alias assumption. Commands and
    # Creative slot changes need not finish in one network scheduling turn.
    before_tool=len(a.events[121])
    for attempt in range(10):
     await command(a,'data get entity Ofwood_ SelectedItem.id')
     if any(('minecraft:'+tool).encode() in payload for _,payload in a.events[121][before_tool:]):break
     await asyncio.sleep(.1)
    else:raise AssertionError('authoritative held tool not installed')
   else:await until(lambda:(36,items['minecraft:'+tool],1) in slot_updates_since(a,0),'tool slot acknowledged')
   await game('survival');await a.conn.send(32,struct.pack('>ff?',0.,0.,True))
   await a.conn.send(30,struct.pack('>ddd?',*a.pos[:3],True))
   before_a,before_b=collected(a),collected(b)
   await destroy(points[half],True);await record('survival-destroy-'+block+'-'+str(half),points,expected=['minecraft:air']*len(points),network_expected=True)
   await command(a,f'tp {C[0]+.5} {C[1]} {C[2]+.5}')
   await until(lambda:collected(a)>before_a and collected(b)>before_b,'one harvested item collected',timeout=12)
   assert collected(a)-before_a==collected(b)-before_b==1
   loot_totals[items['minecraft:'+block]]=loot_totals.get(items['minecraft:'+block],0)+1
   rows.append(dict(case='drop-'+block+'-'+str(half),item=items['minecraft:'+block],count=1))
  await teleport((-16,64,96));await clean();await put()
  await record('door-before-support-break',[C,(C[0],C[1]+1,C[2])],block='oak_door')
  # Keep a different item in the selected slot so pickup cannot be overwritten
  # by the next Creative test setup.
  await hold(a,items['minecraft:stone'])
  await clear_events([C,(C[0],C[1]+1,C[2])]);before_a,before_b=collected(a),collected(b)
  await batch(a,[f'setblock {C[0]} {C[1]-1} {C[2]} air'])
  await record('door-after-support-break',[C,(C[0],C[1]+1,C[2])],expected=['minecraft:air']*2,network_expected=True)
  await batch(a,[f'setblock {C[0]} {C[1]-1} {C[2]} stone']);await command(a,f'tp {C[0]+.5} {C[1]} {C[2]+.5}')
  await until(lambda:collected(a)>before_a and collected(b)>before_b,'support drop collected',timeout=12)
  assert collected(a)-before_a==collected(b)-before_b==1
  loot_totals[items['minecraft:oak_door']]+=1
  await teleport((-16,64,16))
  # Source water stays waterlogged when manually toggled; no fluid simulation
  # comparison is claimed outside the sealed cell.
  await clean();await batch(a,[f'setblock {C[0]+x} {C[1]} {C[2]+z} stone' for x,z in ((1,0),(-1,0),(0,1),(0,-1))]+
                           [f'setblock {" ".join(map(str,C))} water[level=0]'])
  await put('oak_trapdoor');await record('trap-source-water',[C],block='oak_trapdoor')
  for turn in range(2):
   await use(C);water_state=await record('trap-water-use-'+str(turn),[C],block='oak_trapdoor')
  await game('creative');await destroy(C)
  await record('trap-water-destroy',[C],expected=['minecraft:water[level=0]'],network_expected=True)
  await put('oak_trapdoor');water_state=await record('trap-water-replace',[C],block='oak_trapdoor')
  saved.append(dict(point=list(C),state=water_state[0]))
  # Keep real stateful placements at chunk/region boundaries for restarts.
  for point,block in [((-513,64,511),'oak_door'),((-512,64,512),'copper_trapdoor'),((511,64,-513),'waxed_oxidized_copper_door'),((512,64,-512),'iron_trapdoor')]:
   await teleport(point);await clean(radius=0);await put(block,survival=True)
   points=[C,(C[0],C[1]+1,C[2])] if block.endswith('_door') and not block.endswith('_trapdoor') else [C]
   actual=await record('persist-place-'+block,points,block=block)
   if block!='iron_trapdoor':await use(points[-1]);actual=await record('persist-open-'+block,points,block=block)
   saved.extend(dict(point=list(p),state=s) for p,s in zip(points,actual))
  await close(a);a=HingedPlacer(0,opts);a.name='Ofwood_';await a.connect();await a.conn.send(44)
  await until(lambda:bool(a.events[18]),'reconnected inventory')
  slots=latest_inventory(a)
  for item,count in loot_totals.items():assert sum(n for i,(item_id,n) in enumerate(slots) if i!=36 and 9<=i<45 and item_id==item)==count,(item,count,slots)
  Path(args.snapshot).write_text(json.dumps(dict(saved=saved,loot_totals=loot_totals),indent=2))
  print(json.dumps(dict(status='passed',cases=rows,saved=saved,families=len(families),two_observers=True)))
 finally:await asyncio.gather(*(close(p) for p in (a,b) if p.conn and not p.conn.writer.is_closing()),return_exceptions=True)

if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--port',type=int,required=True);p.add_argument('--snapshot',required=True)
 p.add_argument('--blocks-report',default='data/26.1.1/reports/blocks.json');p.add_argument('--ids',default='src/generated/generated_minecraft_ids.json')
 p.add_argument('--core-materials',action='store_true',help='six wood/iron/copper families for secondary-backend regressions');p.add_argument('--verify',action='store_true');p.add_argument('--vanilla',action='store_true');asyncio.run(run(p.parse_args()))
