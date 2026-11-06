#!/usr/bin/env python3
"""Paced Survival workload reusing the existing protocol-775 login/client.
All QA resource grants are explicit test scaffolding; no client or event backlog.
"""
import argparse
import asyncio
from collections import Counter, deque
import json
import math
from pathlib import Path
import signal
import struct
import time
from types import SimpleNamespace
from endurance_common import atomic_json, utc
from protocol import position, read_position, read_varint, string, varint
from stress import Bot, QA_BLOCK

STATE_NAMES={}
FAMILIES=('torch','lantern','oak_slab','oak_stairs','oak_door','oak_trapdoor','ladder','stone')


def lane(index):
    group,within=divmod(index,50)
    return ((group-2)*128+(within%10)*6,64,64+(within//10)*6)


class ActiveBot(Bot):
    def __init__(self,index,args,counts):
        opts=SimpleNamespace(host='127.0.0.1',port=args.port,timeout=30,run_id='END',shared_zone=False,
            mode='connect',qa_verify_movement=False)
        super().__init__(index,opts,lambda _:None)
        self.counts=counts;self.name=f'Endurance{index:05d}';self.target=lane(index)
        self.states={};self.ack=-1;self.reply=None;self.last_active=0;self.latencies=deque(maxlen=256)
        self.family=FAMILIES[index%len(FAMILIES)]
        self.received_updates=0;self.pending=None
        self.walk_x=self.target[0]+(1.5 if self.family=='ladder' else -1.5)

    def block(self,point,state):
        self.received_updates+=1
        if point in (self.target,(self.target[0],self.target[1]+1,self.target[2])):self.states[point]=state

    async def play_packet(self,pid,data):
        if pid==4:self.ack=max(self.ack,read_varint(data)[0])
        elif pid==8:self.block(read_position(data),read_varint(data,8)[0])
        elif pid==84:
            packed=struct.unpack_from('>Q',data)[0];sx,sz,sy=packed>>42,(packed>>20)&0x3fffff,packed&0xfffff
            sx=sx-(1<<22) if sx&(1<<21) else sx;sz=sz-(1<<22) if sz&(1<<21) else sz;sy=sy-(1<<20) if sy&(1<<19) else sy
            count,at=read_varint(data,8)
            for _ in range(count):
                value=0
                for shift in range(0,70,7):
                    byte=data[at];at+=1;value|=(byte&127)<<shift
                    if byte<128:break
                self.block((sx*16+((value>>8)&15),sy*16+(value&15),sz*16+((value>>4)&15)),value>>12)
        elif pid==121 and self.reply:
            predicate,future=self.reply
            if not future.done() and predicate(data):future.set_result(data)
        elif pid==124:self.counts['pickup_packets']+=1
        elif pid==1:self.counts['entity_spawn_packets']+=1 # not unique item-drop count
        elif pid==38 and data[:1]==b'\3':self.gamemode=int(struct.unpack_from('>f',data,1)[0])
        if pid in (32,49,72,45,44,61,11):await super().play_packet(pid,data)

    async def request(self,text,contains):
        future=asyncio.get_running_loop().create_future();self.reply=(lambda data:contains in data,future)
        try:
            await self.conn.send(7,string(text))
            return await asyncio.wait_for(future,30)
        finally:self.reply=None

    async def query_state(self):
        return await self.request('qa state',b'QA state pos=')

    async def prepare(self):
        await self.connect();await self.conn.send(44)
        x,y,z=self.target
        # Test commands create a walking strip and one full support. Creative
        # placement is NOT used for the measured actions.
        await self.conn.send(7,string(f'tp {self.walk_x} 64 {z+.5}'))
        await self.query_state()
        # Match the existing placement harness: load the target neighborhood
        # before editing a cold lane; do not assume a queued setblock succeeded.
        await asyncio.wait_for(asyncio.gather(*(self.chunk_ready_events[(cx,cz)].wait()
            for cx in range(x//16-1,x//16+2) for cz in range(z//16-1,z//16+2))),60)
        commands=[f'setblock {math.floor(self.walk_x)} 63 {z+dz} stone' for dz in range(21)]
        commands += [f'setblock {x} 63 {z} stone',f'setblock {x} 64 {z} air',
            f'setblock {x} 65 {z} air','gamemode survival']
        if self.family=='ladder':commands.append(f'setblock {x-1} 64 {z} stone')
        for text in commands:await self.conn.send(7,string(text))
        observed=await self.query_state();assert b'gm=survival' in observed and b'ready=1' in observed,observed
        await asyncio.wait_for(self.chunk_ready_events[(x//16,z//16)].wait(),30)
        # Reconnection may leave a previous confirmed placement; clear only
        # this bot's dedicated target before a fresh measured cycle.
        await self.conn.send(7,string(f'setblock {x} 64 {z} air'))
        await self.refill()
        for point,expected in [((x,y,z),'minecraft:air'),((x-1,y,z) if self.family=='ladder' else (x,y-1,z),'minecraft:stone')]:
            reply=await self.request('qa block '+' '.join(map(str,point)),b'QA block x=')
            match=QA_BLOCK.search(reply);assert match and STATE_NAMES[int(match.group(4))]==expected,repr(reply)

    async def refill(self):
        await self.conn.send(7,string('qa clear'))
        await self.request('qa give '+self.family+' 64',b'QA give item=')
        await self.request('qa give diamond_pickaxe 1',b'QA give item=')
        await self.conn.send(7,string('food set 20'))
        self.counts['artificial_replenishments']+=1

    async def wait_block(self,point,predicate,sequence):
        end=time.monotonic()+30
        while time.monotonic()<end:
            if self.error or self.state=='closed':raise RuntimeError(self.error or 'connection closed')
            if self.ack>=sequence and point in self.states and predicate(self.states[point]):return self.states[point]
            await asyncio.sleep(.05)
        self.counts['missing_actions']+=1
        raise TimeoutError('state plus sequence ACK')

    async def cycle(self):
        x,y,z=self.target
        # Walk out and back across at least one chunk boundary. Final QA read
        # confirms authoritative position, not every intermediate packet.
        for dz in (3,6,9,12,15,18,15,12,9,6,3,0):
            await self.conn.send(30,struct.pack('>ddd?',self.walk_x,y,z+.5+dz,True))
            self.counts['movements_sent']+=1
            await asyncio.sleep(1)
        await self.conn.send(32,struct.pack('>ff?',90. if self.family=='ladder' else 180.,0.,True))
        await self.conn.send(53,struct.pack('>h',0))
        state=await self.query_state()
        assert ('item=minecraft:'+self.family+' ').encode() in state,repr(state)
        expected=f'pos={self.walk_x:.2f} {y:.2f} {z+.5:.2f}'.encode()
        assert expected in state,(expected,state)
        self.counts['movement_position_checks']+=1
        self.sequence+=1;self.states.pop(self.target,None);started=time.monotonic()
        clicked=(x-1,y,z) if self.family=='ladder' else (x,y-1,z);face=5 if self.family=='ladder' else 1
        self.counts['placements_proposed']+=1;self.pending='placement'
        await self.conn.send(66,varint(0)+position(*clicked)+varint(face)+struct.pack('>fff??',1. if self.family=='ladder' else .5,.5 if self.family=='ladder' else 1.,.5,False,False)+varint(self.sequence))
        self.counts['placements_sent']+=1
        placed=await self.wait_block(self.target,lambda s:s!=0,self.sequence)
        assert STATE_NAMES[placed]=='minecraft:'+self.family,(self.family,placed)
        if self.family=='oak_door':
            await self.wait_block((x,y+1,z),lambda s:STATE_NAMES.get(s)=='minecraft:oak_door',self.sequence)
        self.counts['placements_confirmed']+=1;self.pending=None;self.latencies.append((time.monotonic()-started)*1000)
        if self.family=='stone':
            # Explicit fixture setup, separate from counted client placements:
            # losing this support must remove its attached torch.
            await self.conn.send(7,string(f'setblock {x} {y+1} {z} torch'))
            await self.query_state()
            await self.wait_block((x,y+1,z),lambda s:STATE_NAMES.get(s)=='minecraft:torch',self.sequence)
            self.counts['support_attachments_prepared']+=1
        await self.conn.send(53,struct.pack('>h',1));self.sequence+=1
        self.counts['breaks_proposed']+=1;self.pending='break'
        await self.conn.send(41,varint(0)+position(x,y,z)+varint(1)+varint(self.sequence))
        self.counts['breaks_sent']+=1
        # Surviving non-instant blocks get a normal finish after sufficient time.
        await asyncio.sleep(4)
        self.sequence+=1
        await self.conn.send(41,varint(2)+position(x,y,z)+varint(1)+varint(self.sequence))
        await self.wait_block(self.target,lambda s:s==0,self.sequence)
        if self.family in ('oak_door','stone'):await self.wait_block((x,y+1,z),lambda s:s==0,self.sequence)
        if self.family=='stone':self.counts['support_losses_confirmed']+=1
        self.counts['breaks_confirmed']+=1;self.pending=None
        self.counts['cycles']+=1;self.last_active=time.monotonic()


async def run(args):
    assert args.port==25587 or args.allow_isolated_port,'endurance defaults exclusively to 25587'
    assert args.port!=25565 and 1<=args.count<=25
    if args.admission_origin is None:args.admission_origin=args.first
    assert 0<=args.admission_origin<=args.first and 0<=args.admission_interval<=5
    args.output.mkdir(parents=True,exist_ok=True)
    blocks=json.loads((Path(__file__).resolve().parents[2]/'data/26.1.1/reports/blocks.json').read_text())
    STATE_NAMES.update({state['id']:name for name,block in blocks.items() for state in block['states']})
    counts=Counter();bots={};errors=deque(maxlen=20);stop=asyncio.Event()
    loop=asyncio.get_running_loop()
    for sig in (signal.SIGTERM,signal.SIGINT):loop.add_signal_handler(sig,stop.set)
    async def actor(index):
        await asyncio.sleep((index-args.admission_origin)*args.admission_interval)
        for attempt in range(4):
            bot=ActiveBot(index,args,counts);bots[index]=bot
            try:
                await bot.prepare();counts['connections_succeeded']+=1
                if attempt:counts['reconnections']+=1
                await asyncio.sleep((index%25)*.3)
                while not stop.is_set():
                    start=time.monotonic();await bot.cycle()
                    if counts['cycles'] and bot.sequence%32==0:await bot.refill()
                    await asyncio.sleep(max(.1,args.period-(time.monotonic()-start)))
                return
            except (Exception,asyncio.CancelledError) as exc:
                if isinstance(exc,asyncio.CancelledError):
                    if bot.pending:counts['cancelled_inflight_actions']+=1
                    raise
                if bot.pending:counts['failed_actions']+=1
                counts['disconnects_or_errors']+=1;errors.append(dict(utc=utc(),index=index,error=repr(exc),family=bot.family,pending=bot.pending,
                    target=bot.target,states={str(k):v for k,v in bot.states.items()},ack=bot.ack,sequence=bot.sequence,pos=bot.pos))
            finally:
                if bot.conn:bot.expected_close=True;await bot.conn.close()
                if bot.reader_task:
                    bot.reader_task.cancel();await asyncio.gather(bot.reader_task,return_exceptions=True)
            if stop.is_set():return
            await asyncio.sleep(2**(attempt+1))
        counts['permanent_failures']+=1;stop.set()
    tasks=[asyncio.create_task(actor(i)) for i in range(args.first,args.first+args.count)]
    def report():
        now=time.monotonic();connected=[b for b in bots.values() if b.state=='play' and not b.error]
        latencies=sorted(v for b in bots.values() for v in b.latencies)
        atomic_json(args.output/'status.json',dict(utc=utc(),monotonic=now,pid=__import__('os').getpid(),
            requested=args.count,connected=len(connected),world_ready=sum(bool(b.origin_chunk_at) for b in connected),
            active=sum(now-b.last_active<90 and bool(b.last_active) for b in connected),
            counts=dict(counts),inflight_actions=sum(b.pending is not None for b in bots.values()),families=sorted({b.family for b in connected}),errors=list(errors),
            chunk_packets=sum(b.chunk_packets for b in bots.values()),received_block_updates=sum(b.received_updates for b in bots.values()),
            latency_ms={str(p):latencies[max(0,math.ceil(p*len(latencies))-1)] if latencies else None for p in (.5,.95,.99)},
            network={k:sum(getattr(b.conn,k,0) for b in bots.values() if b.conn) for k in ('sent','received','bytes_sent','bytes_received')}))
    try:
        while not stop.is_set():
            # Explicit test-only resilience trigger, consumed once; never a
            # network command or an automatic disconnect/reconnect loop.
            trigger=args.output/'disconnect-one'
            if trigger.exists():
                trigger.unlink()
                for bot in bots.values():
                    if bot.conn and bot.state=='play':bot.conn.writer.transport.abort();break
            report();await asyncio.sleep(2)
    finally:
        for task in tasks:task.cancel()
        await asyncio.gather(*tasks,return_exceptions=True);report()
    return 1 if counts['permanent_failures'] else 0


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--port',type=int,default=25587);p.add_argument('--allow-isolated-port',action='store_true')
    p.add_argument('--first',type=int,required=True);p.add_argument('--count',type=int,required=True)
    p.add_argument('--admission-origin',type=int);p.add_argument('--admission-interval',type=float,default=1)
    p.add_argument('--period',type=float,default=40);p.add_argument('--output',type=Path,required=True)
    raise SystemExit(asyncio.run(run(p.parse_args())))
