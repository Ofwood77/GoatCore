#!/usr/bin/env python3
"""S3B opaque-data fixture / safe edits / refusal checks, isolated servers only."""
import argparse
import asyncio
import json
from pathlib import Path
import struct
from types import SimpleNamespace
from protocol import position,read_varint,varint
from test_block_placement import Placer,command,setblock,hold,place,observed,report_states,slot_updates_since
from test_block_persistence import query_block
from test_player_visibility import close,until
from test_player_lifecycle import latest_inventory

FIXTURES=[
 ((-15,64,17),'chest[facing=north,type=single,waterlogged=false]','{Items:[{Slot:0b,id:"minecraft:diamond",count:13,components:{"minecraft:custom_data":{S3B:{signed:-17b,array:[I;1,-2,3],nested:[{label:"opaque",value:9L}]}}}}]}'),
 ((-13,64,17),'furnace[facing=north,lit=false]','{Items:[{Slot:0b,id:"minecraft:iron_ore",count:7}]}'),
 ((-11,64,17),'oak_sign[rotation=0,waterlogged=false]','{front_text:{messages:[{text:"S3B"},{text:"ligne"},{text:""},{text:""}],color:"blue",has_glowing_text:1b},is_waxed:1b}'),
 ((-9,64,17),'white_banner[rotation=0]','{patterns:[{pattern:"minecraft:stripe_bottom",color:"red"}]}'),
 ((-7,64,17),'purple_shulker_box[facing=up]','{Items:[{Slot:3b,id:"minecraft:gold_ingot",count:21,components:{"minecraft:custom_data":{source:"S3B",bytes:[B;0b,-1b,42b]}}}]}'),
 ((-5,64,17),'barrel[facing=up,open=false]','{Items:[{Slot:2b,id:"minecraft:emerald",count:19}]}'),
 ((-3,64,17),'decorated_pot[cracked=false,facing=north,waterlogged=false]','{}'),
]
EDIT=(-10,64,26)
async def run(args):
 assert args.port not in (25565,25587)
 state=report_states('data/26.1.1/reports/blocks.json')
 data=json.loads(Path(args.ids).read_text());items={r['name']:r['id'] for r in data['items']}
 opts=SimpleNamespace(host='127.0.0.1',port=args.port,timeout=35,run_id='S3B',shared_zone=False,mode='connect',qa_verify_movement=False)
 a,b=Placer(0,opts),Placer(1,opts);a.name='Ofwood_'
 try:
  await a.connect();await b.connect();await a.conn.send(44);await b.conn.send(44)
  await command(a,'gamemode creative')
  for player,px in ((a,-11.5),(b,-13.5)):
   await command(a,f'tp {player.name} {px} 64 26.5')
  if args.vanilla and not args.verify:
   await command(a,'fill -18 64 14 0 67 30 air')
   await command(a,'fill -18 63 14 0 63 30 stone')
   if args.dense:await command(a,'fill -16 64 16 -1 64 25 chest')
   await command(a,'gamerule minecraft:random_tick_speed 0')
   for point,key,nbt in FIXTURES:
    await setblock(a,(point[0],63,point[2]),'stone')
    await command(a,'setblock '+' '.join(map(str,point))+' minecraft:'+key+nbt)
   await setblock(a,(EDIT[0],63,EDIT[2]),'stone');await setblock(a,EDIT,'air')
   Path(args.snapshot).write_text(json.dumps({'fixture_positions':[list(p) for p,_,_ in FIXTURES],'point':list(EDIT),'block':'minecraft:air'}))
   print(json.dumps(dict(status='passed',official_fixture_entities=len(FIXTURES))));return
  if args.verify:
   snapshot=json.loads(Path(args.snapshot).read_text());expected=snapshot['block'];point=tuple(snapshot['point'])
   if args.vanilla:
    marker='S3B_RESTORED';await command(a,f'execute if block {" ".join(map(str,point))} {expected} run tellraw Ofwood_ "{marker}"')
    await until(lambda:any(marker.encode() in p for _,p in a.events[121]),marker)
   else:assert await query_block(a,point)==state[expected]
   print(json.dumps(dict(status='passed',restart_state=expected)));return
  # Imported entities are opaque: ordinary far-away real Survival placement.
  await hold(a,items['minecraft:stone']);await command(a,'gamemode survival')
  for player in (a,b):player.clear_block_events(EDIT)
  before=len(a.events[20]);await place(a,(EDIT[0],63,EDIT[2]),1,(.5,1.,.5))
  await asyncio.gather(*(observed(p,EDIT,state['minecraft:stone']) for p in (a,b)))
  assert await query_block(a,EDIT)==state['minecraft:stone']
  await until(lambda:(36,items['minecraft:stone'],3) in slot_updates_since(a,before),'far placement consumption')
  await command(a,'gamemode creative')
  # Explicit refusal of removal (including container teardown) and interaction.
  denied=[];entities_before=len(a.events[1])
  for point,key,_ in FIXTURES:
   await command(a,f'tp {point[0]-1.5} 64 {point[2]+.5}')
   await until(lambda:abs(a.pos[0]-(point[0]-1.5))<.01,'teleport')
   await a.conn.send(30,struct.pack('>ddd?',*a.pos,True))
   a.sequence+=1;seq=a.sequence
   await a.conn.send(41,varint(0)+position(*point)+varint(1)+varint(seq))
   await until(lambda:any(read_varint(p)[0]==seq for _,p in a.events[4]),'refused break ACK')
   assert await query_block(a,point)==state['minecraft:'+key]
   opened=len(a.events[59]);await place(a,point,1,(.5,1.,.5));await asyncio.sleep(.08)
   assert len(a.events[59])==opened,'opaque inventory opened'
   await command(a,'setblock '+' '.join(map(str,point))+' air')
   assert await query_block(a,point)==state['minecraft:'+key]
   denied.append(list(point))
  # Source support and a new BE placement are also unsafe; no item consumption.
  point=FIXTURES[0][0];await command(a,f'tp {point[0]-1.5} 64 {point[2]+.5}')
  assert await query_block(a,(point[0],63,point[2]))==state['minecraft:stone']
  await command(a,f'setblock {point[0]} 63 {point[2]} air')
  assert await query_block(a,(point[0],63,point[2]))==state['minecraft:stone']
  assert latest_inventory(a)[36]==(items['minecraft:stone'],3),'refusal changed player inventory'
  assert len(a.events[1])==entities_before,'refusal spawned an entity/drop'
  target=(-10,64,28)
  await command(a,'tp -11.5 64 28.5');await hold(a,items['minecraft:chest']);await command(a,'gamemode survival')
  await place(a,(target[0],63,target[2]),1,(.5,1.,.5))
  assert await query_block(a,target)==state['minecraft:air'],'new native BE created'
  assert latest_inventory(a)[36]==(items['minecraft:chest'],4),'rejected creation consumed item'
  snapshot=dict(fixture_positions=[list(p) for p,_,_ in FIXTURES],point=list(EDIT),block='minecraft:stone',refused=denied)
  Path(args.snapshot).write_text(json.dumps(snapshot,indent=2));print(json.dumps(dict(status='passed',safe_placements=1,refused_entities=denied,observer=b.name)))
 finally:await asyncio.gather(*(close(p) for p in (a,b) if p.conn and not p.conn.writer.is_closing()))
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',type=int,required=True);p.add_argument('--snapshot',required=True);p.add_argument('--dense',action='store_true');p.add_argument('--vanilla',action='store_true');p.add_argument('--verify',action='store_true');p.add_argument('--ids',default='src/generated/generated_minecraft_ids.json')
 args=p.parse_args()
 asyncio.run(run(args))
