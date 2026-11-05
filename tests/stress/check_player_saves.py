#!/usr/bin/env python3
"""Remote-only, disposable player hotbar/periodic/logout/shutdown/crash checks."""
import argparse
import asyncio
import csv
import gzip
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import time
from types import SimpleNamespace
import uuid

from protocol import varint
from test_player_lifecycle import Player, command, latest_inventory
from test_player_visibility import close, offline_uuid, until
from native_storage_oracle import parse, integer

ROOT = Path(__file__).resolve().parents[2]


def saved(world, name):
    path = world / 'players/data' / (str(uuid.UUID(bytes=offline_uuid(name))) + '.dat')
    return integer(parse(gzip.decompress(path.read_bytes())), 'SelectedItemSlot')


async def barrier(bot, selected=None):
    before = len(bot.events[121])
    await command(bot, 'qa state' if selected is not None else 'mineinfo')
    await until(lambda: len(bot.events[121]) > before, 'ordered read-only response', timeout=30)
    if selected is not None:
        assert any(f'hotbar={36+selected} '.encode() in raw for _, raw in bot.events[121][before:])


class Instance:
    def __init__(self, args, label, gate=False):
        self.args = args
        self.root = args.result_dir / label
        self.root.mkdir()
        self.runtime = self.root / 'runtime'
        self.runtime.mkdir()
        (self.runtime / 'assets').symlink_to(ROOT / 'assets')
        (self.runtime / 'server.config').write_text(f'memory-budget-mib={getattr(args,"memory_budget_mib",4096)}\nonline-mode=false\noffline-skins=false\nchunk-storage=goatworld-sqlite-experimental\nchunk-save-mode=async\nview-distance=2\n')
        self.world = self.runtime / 'world'
        self.gate = gate
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            self.port = listener.getsockname()[1]
        assert self.port not in (25565, 25587)
        self.phase = 0
        self.proc = None

    async def start(self):
        self.phase += 1
        env = dict(os.environ, MC_WORLD_PATH=str(self.world), MC_BIND_PORT=str(self.port),
            MC_SETUP_FILE=str(ROOT/'tests/stress/network/one.conf'), MC_QA_MODE='1', MC_ADMIN_USERNAME='Ofwood_',
            MC_SAVE_TRACE_PATH=str(self.root/f'saves-{self.phase}.csv'))
        if self.args.fsync_probe:
            env.update(LD_PRELOAD=str(self.args.fsync_probe), MC_FSYNC_TRACE_PATH=str(self.root/f'fsync-{self.phase}.csv'))
        if self.gate:
            env.update(LD_PRELOAD=str(self.args.gate_probe), MC_PLAYER_GATE_DIR=str(self.root))
        if self.root.name=='mixed-inventory':env['MC_DEBUG_ITEMS']='1'
        with (self.root/f'server-{self.phase}.log').open('w') as log:
            self.proc = subprocess.Popen([self.args.binary], cwd=self.runtime, env=env, stdout=log, stderr=subprocess.STDOUT)
        for _ in range(600):
            assert self.proc.poll() is None, 'startup failed'
            try:
                with socket.create_connection(('127.0.0.1', self.port), .1): return
            except OSError: await asyncio.sleep(.1)
        raise TimeoutError('isolated server startup')

    async def stop(self, kill=False, expected=0):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL if kill else signal.SIGTERM)
        rc = await asyncio.wait_for(asyncio.to_thread(self.proc.wait), 120)
        assert rc == (-signal.SIGKILL if kill else expected), (rc, expected)

    async def clients(self, count, kind=Player):
        opts = SimpleNamespace(host='127.0.0.1', port=self.port, timeout=60, run_id='HOTS',
            shared_zone=False, mode='connect', qa_verify_movement=False)
        actors = [kind(i, opts) for i in range(count)]
        actors[0].name = 'Ofwood_'
        for bot in actors:
            await bot.connect()
            await bot.conn.send(44)
            await until(lambda: bot.events[105], 'restored selected slot')
        return actors


async def batch_case(args, count):
    instance = Instance(args, f'hotbar-{count}')
    actors = []
    try:
        await instance.start()
        actors = await instance.clients(count)
        for bot in actors:
            await bot.conn.send(56, struct.pack('>h',36)+varint(7)+varint(1)+b'\0\0')
            await barrier(bot)
            assert saved(instance.world, bot.name) == 0
        start = time.monotonic_ns()//1000
        async def changes(bot, index):
            for step in range(1,101):
                slot = (step+index)%9
                await bot.conn.send(53, struct.pack('>h',slot))
                await barrier(bot, slot if index == 0 else None)
            for _ in range(8): await bot.conn.send(53, struct.pack('>h',slot))
            for invalid in (-1,9): await bot.conn.send(53, struct.pack('>h',invalid))
            await barrier(bot, slot if index == 0 else None)
        await asyncio.gather(*(changes(bot,i) for i,bot in enumerate(actors)))
        end = time.monotonic_ns()//1000
        assert end-start < 25_000_000, 'test did not finish within the deferred window'
        assert all(saved(instance.world, bot.name) == 0 for bot in actors), 'unexpected eager hotbar save'
        periodic = count == 1
        if periodic:
            await until(lambda: saved(instance.world, actors[0].name) == 1, '30 second periodic hotbar flush', timeout=35)
        await asyncio.gather(*(close(bot) for bot in actors))
        await until(lambda: all(saved(instance.world, bot.name) == (100+i)%9 for i,bot in enumerate(actors)), 'departure flush')
        actors = await instance.clients(count)
        for i, bot in enumerate(actors):
            assert bot.events[105][-1][1] == bytes([(100+i)%9])
            assert latest_inventory(bot)[36] == (1,7)
            await bot.conn.send(53, struct.pack('>h',(101+i)%9))
            await barrier(bot, (101+i)%9 if i == 0 else None)
        await instance.stop()  # clients still connected and dirty
        await asyncio.gather(*(close(bot) for bot in actors), return_exceptions=True)
        for i,bot in enumerate(actors): assert saved(instance.world, bot.name) == (101+i)%9
        await instance.start()
        actors = await instance.clients(count)
        for i,bot in enumerate(actors):
            assert bot.events[105][-1][1] == bytes([(101+i)%9])
            assert latest_inventory(bot)[36] == (1,7)
        await asyncio.gather(*(close(bot) for bot in actors))
        await instance.stop()
        with (instance.root/'saves-1.csv').open() as source:
            active = [r for r in csv.DictReader(source) if start <= int(r['start_us']) < end]
        assert sum(r['phase']=='player_hotbar' for r in active) == 108*count
        assert not any(r['phase']=='player_file_save' for r in active)
        proof = dict(status='passed', players=count, changes=100*count, no_op_packets=8*count,
            invalid_packets=2*count, immediate_player_files=0, periodic_flush=periodic,
            logout_reconnect=True, connected_dirty_shutdown=True, restart_inventory=True)
        (instance.root/'proof.json').write_text(json.dumps(proof,indent=2)+'\n')
        return proof
    finally:
        await asyncio.gather(*(close(bot) for bot in actors if bot.conn),return_exceptions=True)
        if instance.proc and instance.proc.poll() is None: await instance.stop()


async def crash_case(args, fail_path):
    instance = Instance(args, 'failed-departure' if fail_path else 'sigkill-memory')
    actors = []
    backup = None
    try:
        await instance.start()
        actors = await instance.clients(1)
        bot = actors[0]
        await bot.conn.send(56, struct.pack('>h',36)+varint(7)+varint(1)+b'\0\0')
        await barrier(bot)
        await bot.conn.send(53, struct.pack('>h',4)); await barrier(bot,4)
        assert saved(instance.world, bot.name)==0
        if fail_path:
            data=instance.world/'players/data';backup=data.with_name('data-preserved')
            data.rename(backup);data.write_text('injected ENOTDIR, disposable test only\n')
            await close(bot)
            assert await asyncio.wait_for(asyncio.to_thread(instance.proc.wait),30)==2
            data.unlink();backup.rename(data);backup=None
            assert 'NOT durably saved' in (instance.root/'server-1.log').read_text()
        else: await instance.stop(kill=True)
        await asyncio.gather(*(close(b) for b in actors),return_exceptions=True)
        assert saved(instance.world,bot.name)==0
        await instance.start();actors=await instance.clients(1)
        assert actors[0].events[105][-1][1]==b'\0' and latest_inventory(actors[0])[36]==(1,7)
        await close(actors[0]);await instance.stop()
        return dict(status='passed',case='failed-departure' if fail_path else 'SIGKILL-memory',old_inventory_intact=True,old_slot=0)
    finally:
        await asyncio.gather(*(close(b) for b in actors if b.conn),return_exceptions=True)
        if instance.proc and instance.proc.poll() is None:await instance.stop(expected=2 if fail_path else 0)
        if backup:
            (instance.world/'players/data').unlink();backup.rename(instance.world/'players/data')


async def gate_case(args):
    instance=Instance(args,'blocked-write',gate=True);actors=[]
    try:
        await instance.start();actors=await instance.clients(1);old=actors[0]
        await old.conn.send(53,struct.pack('>h',2));await barrier(old,2)
        (instance.root/'arm').touch()
        await old.conn.send(56,struct.pack('>h',36)+varint(9)+varint(1)+b'\0\0')
        await until(lambda:(instance.root/'ready').exists(),'file synced before publication',timeout=30)
        await close(old)
        joining=asyncio.create_task(instance.clients(1))
        await asyncio.sleep(.2);assert not joining.done(),'login must wait for authoritative tick/save'
        (instance.root/'release').touch();actors=await asyncio.wait_for(joining,30)
        assert actors[0].events[105][-1][1]==b'\2' and latest_inventory(actors[0])[36]==(1,9)
        await close(actors[0]);await instance.stop()
        for name in ('arm','ready','release'):(instance.root/name).unlink()
        await instance.start();actors=await instance.clients(1)
        await actors[0].conn.send(53,struct.pack('>h',4));await barrier(actors[0],4)
        (instance.root/'arm').touch()
        await actors[0].conn.send(56,struct.pack('>h',36)+varint(11)+varint(1)+b'\0\0')
        await until(lambda:(instance.root/'ready').exists(),'shutdown gate',timeout=30)
        instance.proc.send_signal(signal.SIGTERM)
        await asyncio.sleep(.2);(instance.root/'release').touch()
        await instance.stop()
        await asyncio.gather(*(close(b) for b in actors),return_exceptions=True)
        for name in ('arm','ready','release'):(instance.root/name).unlink()
        await instance.start();actors=await instance.clients(1)
        assert actors[0].events[105][-1][1]==b'\4' and latest_inventory(actors[0])[36]==(1,11)
        await close(actors[0]);await instance.stop()
        return dict(status='passed',case='blocked-write',same_uuid_reconnect=True,shutdown_during_write=True,inventory=11,selected=4)
    finally:
        (instance.root/'release').touch()
        await asyncio.gather(*(close(b) for b in actors if b.conn),return_exceptions=True)
        if instance.proc and instance.proc.poll() is None:await instance.stop()


async def mixed_case(args):
    from protocol import position, read_varint
    from test_pickup_save import Collector, collected
    from test_block_placement import place, observed, setblock, slot_updates_since
    instance=Instance(args,'mixed-inventory');actors=[]
    ids={row['name']:row['id'] for row in json.loads((ROOT/'src/generated/generated_minecraft_ids.json').read_text())['items']}
    bread=ids['minecraft:bread'];point=(-16,64,16)
    try:
        await instance.start();actors=await instance.clients(2,Collector);a,b=actors
        for bot,x in ((a,-18.5),(b,-20.5)):
            await command(a,f'tp {bot.name} {x} 64 16.5')
            await until(lambda:bot.pos and abs(bot.pos[0]-x)<.01,'mixed teleport',timeout=60)
            await bot.conn.send(30,struct.pack('>ddd?',x,64.,16.5,True))
        await setblock(a,(point[0],63,point[2]),'stone');await setblock(a,point,'air')
        for slot,item,count in ((36,1,4),(37,bread,3)):
            await a.conn.send(56,struct.pack('>h',slot)+varint(count)+varint(item)+b'\0\0')
        await command(a,'gamemode survival');await command(a,'food set 12');await barrier(a,0)
        await place(a,(point[0],63,point[2]),1,(.5,.5,.5))
        await observed(b,point,1)
        await until(lambda:(36,1,3) in slot_updates_since(a,0),'Survival placement consumption')
        await a.conn.send(53,struct.pack('>h',1));await barrier(a,1)
        a.sequence+=1
        await a.conn.send(67,varint(0)+varint(a.sequence)+struct.pack('>ff',0.,0.))
        await until(lambda:(37,bread,2) in slot_updates_since(a,0),'food consumed and synchronized',timeout=12)
        await a.conn.send(53,struct.pack('>h',0));await barrier(a,0)
        picked=collected(a);observer_picked=collected(b);a.sequence+=1
        await a.conn.send(41,varint(4)+position(0,0,0)+b'\0'+varint(a.sequence))
        # Persistence test, not projectile physics: follow the actual position
        # broadcast for the dropped entity, after its manual pickup delay.
        item_kind=next(row['id'] for row in json.loads((ROOT/'src/generated/generated_minecraft_ids.json').read_text())['entity_types'] if row['name']=='minecraft:item')
        await asyncio.sleep(2.2)
        entity=None
        for _,payload in a.events[1]:
            candidate,offset=read_varint(payload);kind,_=read_varint(payload,offset+16)
            if kind==item_kind:entity=candidate
        assert entity is not None,'manual item entity not observed'
        last=None
        for _,payload in a.events[125]:
            candidate,offset=read_varint(payload)
            if candidate==entity:last=struct.unpack_from('>ddd',payload,offset)
        assert last is not None,'no authoritative item position'
        (instance.root/'drop-position.json').write_text(json.dumps(dict(entity=entity,position=last))+'\n')
        await command(a,'tp '+' '.join(map(str,last)))
        await until(lambda:collected(a)>picked and collected(b)>observer_picked,'one dropped item picked up',timeout=15)
        assert collected(a)-picked==collected(b)-observer_picked==1
        await until(lambda:latest_inventory(a)[36]==(1,3),'inventory after pickup')
        await asyncio.gather(*(close(bot) for bot in actors))
        actors=await instance.clients(1,Collector)
        assert latest_inventory(actors[0])[36]==(1,3) and latest_inventory(actors[0])[37]==(bread,2)
        assert actors[0].events[105][-1][1]==b'\0'
        await close(actors[0]);await instance.stop();await instance.start();actors=await instance.clients(1,Collector)
        assert latest_inventory(actors[0])[36]==(1,3) and latest_inventory(actors[0])[37]==(bread,2)
        await close(actors[0]);await instance.stop()
        return dict(status='passed',case='mixed-inventory',survival_placement=True,food_consumed=1,
            dropped_and_collected=1,two_observers=True,reconnect=True,restart=True,stone=3,bread=2)
    finally:
        await asyncio.gather(*(close(bot) for bot in actors if bot.conn),return_exceptions=True)
        if instance.proc and instance.proc.poll() is None:await instance.stop()


async def run(args):
    args.binary=args.binary.resolve();args.result_dir=args.result_dir.resolve()
    if args.fsync_probe:args.fsync_probe=args.fsync_probe.resolve()
    if args.gate_probe:args.gate_probe=args.gate_probe.resolve()
    args.result_dir.mkdir(parents=True,exist_ok=False)
    rows=[]
    if not args.mixed_only:
        rows=[await batch_case(args,n) for n in args.counts]
        rows.extend([await crash_case(args,False),await crash_case(args,True)])
        if args.gate_probe:rows.append(await gate_case(args))
    rows.append(await mixed_case(args))
    proof=dict(status='passed',server_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),cases=rows)
    (args.result_dir/'proof.json').write_text(json.dumps(proof,indent=2)+'\n');print(json.dumps(proof))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--result-dir',type=Path,required=True)
    parser.add_argument('--counts',type=int,nargs='+',default=[1,10])
    parser.add_argument('--fsync-probe',type=Path)
    parser.add_argument('--gate-probe',type=Path)
    parser.add_argument('--memory-budget-mib',type=int,default=4096)
    parser.add_argument('--mixed-only',action='store_true')
    asyncio.run(run(parser.parse_args()))
