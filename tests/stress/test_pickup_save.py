#!/usr/bin/env python3
"""Remote-only pickup inventory/count comparison and clean restart invariant."""
import argparse
import asyncio
import json
from pathlib import Path
import struct
import time
from types import SimpleNamespace

from protocol import position, varint, read_varint
from test_block_placement import Placer, hold, command, setblock, slot_updates_since
from test_block_persistence import query_block
from test_player_lifecycle import latest_inventory
from test_player_visibility import close, until


class Collector(Placer):
    async def play_packet(self, packet_id, payload):
        if packet_id==124:self.events[packet_id].append((asyncio.get_running_loop().time(),payload))
        await super().play_packet(packet_id,payload)


def collected(player):
    total=0
    for _,payload in player.events[124]:
        _,offset=read_varint(payload)
        _,offset=read_varint(payload,offset)
        count,end=read_varint(payload,offset)
        assert end==len(payload)
        total+=count
    return total


async def run(args):
    assert args.port not in (25565,25587)
    ids=json.loads(Path('src/generated/generated_minecraft_ids.json').read_text())['items']
    stone=next(i['id'] for i in ids if i['name']=='minecraft:stone')
    opts=SimpleNamespace(host='127.0.0.1',port=args.port,timeout=30,run_id='PICK',shared_zone=False,
                         mode='connect',qa_verify_movement=False)
    a,b=Collector(0,opts),Collector(1,opts);a.name='Ofwood_'
    try:
        await a.connect();await b.connect()
        await a.conn.send(44);await b.conn.send(44)
        if args.verify:
            expected=json.loads(Path(args.snapshot).read_text())
            slots=latest_inventory(a)
            if args.drop_count==2:assert slots[36]==(stone,expected['inventory_count'])
            else:assert sum(n for item,n in slots[9:45] if item==stone)==100
            print(json.dumps({'status':'passed','inventory_count':expected['inventory_count']}));return
        await setblock(a,(-19,63,14),'stone');await setblock(a,(-21,63,14),'stone')
        await command(a,'tp -18.5 64 14.5');await command(a,f'tp {b.name} -20.5 64 14.5')
        if args.drop_count>2 and not args.vanilla:
            await asyncio.wait_for(asyncio.gather(*(p.chunk_ready_events[(x,z)].wait()
                for p in (a,b) for x in range(-12,9) for z in range(-10,11))),60)
            await asyncio.sleep(1) # chunk streaming is preparation, not pickup work
        await hold(a,stone)
        if args.drop_count>2:
            for slot,count in ((36,64),(37,36)):
                await a.conn.send(56,struct.pack('>h',slot)+varint(count)+varint(stone)+b'\0\0')
            await asyncio.sleep(.2)
        await command(a,'gamemode survival')
        await a.conn.send(32,struct.pack('>ff?',0.,90.,True))
        before=len(a.events[20])
        for i in range(args.drop_count):
            if i==64:await a.conn.send(53,struct.pack('>h',1))
            a.sequence+=1
            await a.conn.send(41,varint(4)+position(0,0,0)+varint(0)+varint(a.sequence))
        if args.drop_count>2 and not args.vanilla:
            await until(lambda:all(any(slot==s and count==0 for slot,item,count in slot_updates_since(a,before)) for s in (36,37)),
                        'all 100 drops confirmed',timeout=40)
            await query_block(a,(-19,63,14)) # preparation barrier, no drop save left in flight
        if args.drop_count>2:assert collected(a)==collected(b)==0, 'pickup started during preparation'
        active_start=time.monotonic_ns()//1000
        await until(lambda:collected(a)>=args.drop_count and collected(b)>=args.drop_count,'dropped items collected',timeout=40)
        assert collected(a)==collected(b)==args.drop_count
        if args.drop_count>2 and not args.vanilla:await query_block(a,(-19,63,14)) # end-of-pass commit barrier
        active_end=time.monotonic_ns()//1000
        # Reconnect without Creative inventory resets; check the authoritative
        # full inventory and later a fresh process reading the same save.
        await close(a)
        a=Collector(0,opts);a.name='Ofwood_'
        await a.connect();await a.conn.send(44)
        await until(lambda:bool(a.events[18]),'saved pickup inventory')
        slots=latest_inventory(a)
        if args.drop_count==2:assert slots[36]==(stone,4)
        else:assert sum(n for item,n in slots[9:45] if item==stone)==100
        report={'picked_count':args.drop_count,'observer_picked_count':args.drop_count,
                'inventory_count':4 if args.drop_count==2 else 100,'item':'minecraft:stone'}
        if args.drop_count>2:report['active_us']=[active_start,active_end]
        Path(args.snapshot).write_text(json.dumps(report,indent=2))
        print(json.dumps({'status':'passed',**report}))
    finally:
        await asyncio.gather(*(close(p) for p in (a,b) if p.conn and not p.conn.writer.is_closing()),return_exceptions=True)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port',type=int,required=True)
    p.add_argument('--snapshot',required=True)
    p.add_argument('--verify',action='store_true')
    p.add_argument('--vanilla',action='store_true',help='same observable invariants; no GoatCore QA needed')
    p.add_argument('--drop-count',type=int,default=2,choices=(2,100))
    asyncio.run(run(p.parse_args()))
