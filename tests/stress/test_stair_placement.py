#!/usr/bin/env python3
"""Remote-only differential stair states; run on isolated offline instances.

Same actions for Vanilla/GoatCore, authoritative reads plus a second observer.
Use --verify with the same snapshot after a clean server restart.
"""
import argparse
import asyncio
import json
from pathlib import Path
import struct
from types import SimpleNamespace

from protocol import read_varint, string, varint
from test_block_placement import Placer, hold, command, setblock, place, observed, report_states, slot_updates_since
from test_block_persistence import query_block
from test_player_visibility import close, until
from test_player_lifecycle import latest_inventory
from protocol import position

DIR = ["north", "east", "south", "west"]
DX, DZ = [0, 1, 0, -1], [-1, 0, 1, 0]
C = (-16, 64, 16)


def key(facing, half="bottom", shape="straight", wet=False, block="oak_stairs"):
    return f"minecraft:{block}[facing={DIR[facing]},half={half},shape={shape},waterlogged={str(wet).lower()}]"


async def batch(bot, commands):
    for text in commands:
        await bot.conn.send(7, string(text))
    await asyncio.sleep(0.2)


async def reset(bot):
    await batch(bot, [f"setblock {x} {y} {z} {'stone' if y==63 else 'air'}"
                      for x in range(-18, -13) for z in range(14, 19) for y in (63,64,65)])


async def world_is(bot, point, state_key, state_id, vanilla, tag):
    if vanilla:
        marker = "STAIR_" + tag
        await command(bot, f"execute if block {' '.join(map(str,point))} {state_key} run tellraw Ofwood_ \"{marker}\"")
        await until(lambda: any(marker.encode() in p for _, p in bot.events[121]), marker)
    else:
        assert await query_block(bot, point) == state_id, (tag, point, state_key)


async def run(args):
    assert args.port not in (25565,25587), "temporary instances only"
    states = report_states(args.blocks_report)
    names = {v:k for k,v in states.items()}
    items = {i["name"]:i["id"] for i in json.loads(Path(args.ids).read_text())["items"]}
    opts = SimpleNamespace(host=args.host, port=args.port, timeout=35, run_id="STAIR",
                           shared_zone=False, mode="connect", qa_verify_movement=False)
    a,b = Placer(0,opts), Placer(1,opts)
    a.name = "Ofwood_"
    rows=[]
    async def check(tag, point, expected, network=True):
        value=states[expected]
        if network:
            await asyncio.gather(observed(a,point,value),observed(b,point,value))
        await world_is(a,point,expected,value,args.vanilla,tag)
        rows.append({"case":tag,"point":list(point),"block_state":expected})
    async def action(point,facing,half="bottom",face=None,hit_y=None,survival=False):
        await hold(a,items["minecraft:oak_stairs"])
        if survival: await command(a,"gamemode survival")
        await a.conn.send(32,struct.pack(">ff?",float(((facing+2)&3)*90),0.,True))
        # 775 MovePlayerRot is packet 32; each facing is chosen by the server.
        if face is None: face=0 if half=="top" else 1
        if hit_y is None: hit_y=0 if face==0 else 1 if face==1 else .75 if half=="top" else .25
        offsets={0:(0,1,0),1:(0,-1,0),2:(0,0,1),3:(0,0,-1),4:(1,0,0),5:(-1,0,0)}
        clicked=tuple(p+d for p,d in zip(point,offsets[face]))
        await setblock(a,clicked,"stone")
        for p in (a,b):
            p.clear_block_events(C);p.clear_block_events(point)
        before=len(a.events[20])
        hit=(0 if face==4 else 1 if face==5 else .5,hit_y,0 if face==2 else 1 if face==3 else .5)
        await place(a,clicked,face,hit)
        if survival:
            await until(lambda:(36,items["minecraft:oak_stairs"],3) in slot_updates_since(a,before),"stair Survival consumption")
    try:
        await a.connect();await b.connect()
        await a.conn.send(44);await b.conn.send(44)
        if args.verify:
            saved=json.loads(Path(args.snapshot).read_text())
            for i,row in enumerate(saved):
                await world_is(a,tuple(row["point"]),row["block_state"],states[row["block_state"]],args.vanilla,f"restart_{i}")
            print(json.dumps({"status":"passed","restart_states":len(saved)}));return
        await batch(a,["setblock -19 63 14 stone","setblock -21 63 14 stone"])
        await command(a,"tp -18.5 64 14.5")
        await command(a,f"tp {b.name} -20.5 64 14.5")
        for cx in (-2,-1):
            for cz in (0,1): await asyncio.wait_for(a.chunk_ready_events[(cx,cz)].wait(),35)
        a.sequence=126
        for f in range(4):
            for half in ("bottom","top"):
                await reset(a)
                await action(C,f,half,survival=True)
                await check(f"facing_{DIR[f]}_{half}",C,key(f,half))
        for face in (2,3,4,5):
            for h in (.25,.5,.75):
                await reset(a)
                half="top" if h>.5 else "bottom"
                await action(C,1,half,face,h)
                await check(f"face_{face}_hit_{h}",C,key(1,half))
        for f in range(4):
            for half in ("bottom","top"):
                for back in (False,True):
                    for right in (False,True):
                        tag=f"{DIR[f]}_{half}_{'inner' if back else 'outer'}_{'right' if right else 'left'}"
                        await reset(a)
                        # Initial center is placed through the real packet.
                        await action(C,f,half)
                        await check(tag+"_initial",C,key(f,half))
                        n=(f+(1 if right else 3))&3
                        d=(f+2)&3 if back else f
                        near=(C[0]+DX[d],C[1],C[2]+DZ[d])
                        await action(near,n,half)
                        shape=("inner_" if back else "outer_")+("right" if right else "left")
                        await check(tag+"_paired",C,key(f,half,shape))
                        # Change orientation with /setblock (Minecraft has no generic stair wrench).
                        for p in (a,b):p.clear_block_events(C)
                        await setblock(a,near,key(f,half))
                        await check(tag+"_rotated",C,key(f,half))
                        for p in (a,b):p.clear_block_events(C)
                        await setblock(a,near,key(n,half))
                        await check(tag+"_restored",C,key(f,half,shape))
                        for p in (a,b):p.clear_block_events(C)
                        a.sequence+=1
                        await a.conn.send(41,varint(0)+position(*near)+varint(1)+varint(a.sequence))
                        await check(tag+"_removed",C,key(f,half))
        # Side veto, mixed materials and different halves; direct fixture writes
        # also exercise the world hook independently of UseItemOn.
        for half in ("bottom","top"):
            for p in (a,b):p.clear_block_events(C)
            await reset(a)
            await setblock(a,C,key(0,half))
            await setblock(a,(-16,64,15),key(3,half,block="stone_stairs"))
            await check("mixed_"+half,C,key(0,half,"outer_left"))
            for p in (a,b):p.clear_block_events(C)
            await setblock(a,(-15,64,16),key(0,half))
            await check("veto_"+half,C,key(0,half))
            for p in (a,b):p.clear_block_events(C)
            await setblock(a,(-15,64,16),key(0,"top" if half=="bottom" else "bottom"))
            await check("opposite_half_guard_"+half,C,key(0,half,"outer_left"))
        await reset(a)
        await setblock(a,C,key(0))
        await setblock(a,(-16,64,17),key(3))
        for p in (a,b):p.clear_block_events(C)
        await setblock(a,(-16,64,15),key(3))
        await check("outer_precedes_inner",C,key(0,shape="outer_left"))
        for p in (a,b):p.clear_block_events(C)
        await setblock(a,(-15,64,16),key(0))
        await check("veto_outer_falls_back_to_inner",C,key(0,shape="inner_left"))
        await reset(a)
        # Source water is confined; scheduled fluid behavior is outside this lot.
        await batch(a,[f"setblock {C[0]+DX[d]} 64 {C[2]+DZ[d]} stone" for d in range(4)])
        await setblock(a,C,"water[level=0]")
        await action(C,2)
        await check("source_water",C,key(2,wet=True))
        # Existing rejection policy must preserve an occupied target.
        await reset(a);await setblock(a,C,"stone")
        await hold(a,items["minecraft:oak_stairs"]);await command(a,"gamemode survival")
        for p in (a,b):p.clear_block_events(C)
        await place(a,(C[0],63,C[2]),1,(.5,1,.5))
        await check("occupied",C,"minecraft:stone",network=False)
        before=len(a.events[20])
        # Full lower half blocks placement inside the player's feet.
        own=(-19,64,14)
        await setblock(a,own,"air")
        await place(a,(-19,63,14),1,(.5,1,.5))
        await check("self_collision",own,"minecraft:air",network=False)
        assert all(count==4 for slot,item,count in slot_updates_since(a,before) if slot==36)
        # A refusal need not send a slot update on Vanilla. Reconnect without
        # resetting the stack to read the authoritative inventory after BOTH
        # rejected actions, rather than treating an empty update list as proof.
        await close(a)
        a=Placer(0,opts);a.name="Ofwood_"
        await a.connect();await a.conn.send(44)
        await until(lambda:bool(a.events[18]),"inventory after refused placements")
        assert latest_inventory(a)[36]==(items["minecraft:oak_stairs"],4)
        # The upper empty half of a north-facing stair must remain empty.
        await hold(a,items["minecraft:oak_stairs"])
        await command(a,"tp -18.5 64.5 14.8")
        await a.conn.send(32,struct.pack(">ff?",180.,0.,True))
        for p in (a,b):p.clear_block_events(own)
        await place(a,(-19,63,14),1,(.5,1,.5))
        await check("partial_voxel_collision",own,key(0))
        await setblock(a,own,"air")
        await command(a,"tp -18.5 64 14.5")
        # Leave all four corner kinds plus straight, both halves and two
        # chunk boundaries, produced by actual placement packets.
        await reset(a)
        saved=[]
        placements=[((-17,64,15),1,"bottom"),((-16,64,15),0,"bottom"),
                    ((-17,64,17),1,"bottom"),((-18,64,17),0,"bottom"),
                    ((-17,66,15),1,"top"),((-16,66,15),2,"top"),
                    ((-17,66,17),1,"top"),((-18,66,17),2,"top")]
        await batch(a,[f"setblock {x} {y} {z} air" for (x,y,z),_,_ in placements])
        for point,f,half in placements:await action(point,f,half)
        expected=[key(1,shape="outer_left"),key(0),key(1,shape="inner_left"),key(0),
                  key(1,"top","outer_right"),key(2,"top"),key(1,"top","inner_right"),key(2,"top")]
        for (point,_,_),k in zip(placements,expected):
            await world_is(a,point,k,states[k],args.vanilla,"save_"+str(len(saved)))
            saved.append({"point":list(point),"block_state":k})
        Path(args.snapshot).write_text(json.dumps(saved,indent=2))
        result={"status":"passed","cases":rows,"saved":saved}
        if args.report:Path(args.report).write_text(json.dumps(result,indent=2))
        print(json.dumps(result))
    finally:
        await asyncio.gather(*(close(p) for p in (a,b) if p.conn and not p.conn.writer.is_closing()))


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host",default="127.0.0.1")
    parser.add_argument("--port",type=int,required=True)
    parser.add_argument("--blocks-report",default="data/26.1.1/reports/blocks.json")
    parser.add_argument("--ids",default="src/generated/generated_minecraft_ids.json")
    parser.add_argument("--snapshot",required=True)
    parser.add_argument("--report")
    parser.add_argument("--verify",action="store_true")
    parser.add_argument("--vanilla",action="store_true")
    asyncio.run(run(parser.parse_args()))
