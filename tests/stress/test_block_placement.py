#!/usr/bin/env python3
"""Run identical protocol-775 placements on disposable remote GoatCore/Vanilla.

Requires operator Ofwood_ and Creative start. Raw report supplies every expected
state ID; no old-version constants. Never target 25565. Use --verify after restart.
"""
import argparse
import asyncio
import json
import hashlib
from pathlib import Path
import struct
from types import SimpleNamespace

from protocol import position, read_varint, string, varint
from test_player_lifecycle import Player, slot_updates, latest_inventory
from test_player_visibility import close, until
from test_block_persistence import query_block


class Placer(Player):
    async def play_packet(self, packet_id, payload):
        if packet_id == 84:
            packed = struct.unpack_from(">Q", payload)[0]
            sx, sz, sy = packed >> 42, (packed >> 20) & 0x3fffff, packed & 0xfffff
            sx = sx - (1 << 22) if sx & (1 << 21) else sx
            sz = sz - (1 << 22) if sz & (1 << 21) else sz
            sy = sy - (1 << 20) if sy & (1 << 19) else sy
            count, offset = read_varint(payload, 8)
            for _ in range(count):
                entry = 0
                for shift in range(0, 70, 7):
                    byte = payload[offset]
                    offset += 1
                    entry |= (byte & 127) << shift
                    if not byte & 128: break
                point = (sx*16 + ((entry >> 8) & 15), sy*16 + (entry & 15), sz*16 + ((entry >> 4) & 15))
                self.block_events[point].put_nowait(entry >> 12)
        if packet_id in (4, 7, 59):
            self.events[packet_id].append((asyncio.get_running_loop().time(), payload))
        await super().play_packet(packet_id, payload)


def report_states(path):
    data = json.loads(Path(path).read_text())
    return {name + ("[" + ",".join(f"{k}={v}" for k, v in sorted(s["properties"].items())) + "]"
                     if s.get("properties") else ""): s["id"]
            for name, block in data.items() for s in block["states"]}


async def command(bot, text):
    await bot.conn.send(7, string(text))
    await asyncio.sleep(0.12)


async def setblock(bot, point, key):
    await command(bot, f"setblock {' '.join(map(str, point))} {key}")


async def hold(bot, item):
    await command(bot, "gamemode creative")
    await bot.conn.send(56, struct.pack(">h", 36) + varint(4) + varint(item) + b"\0\0")
    await bot.conn.send(53, struct.pack(">h", 0))
    await asyncio.sleep(0.15)


async def place(bot, clicked, face, hit=(0.5, 0.5, 0.5)):
    bot.sequence += 1
    await bot.conn.send(66, varint(0) + position(*clicked) + varint(face) +
                        struct.pack(">fff??", *hit, False, False) + varint(bot.sequence))
    await until(lambda: any(read_varint(p)[0] == bot.sequence for _, p in bot.events[4]),
                f"ACK sequence {bot.sequence}")


async def observed(bot, point, state):
    deadline = asyncio.get_running_loop().time() + 8
    while asyncio.get_running_loop().time() < deadline:
        value = await asyncio.wait_for(bot.block_events[point].get(), 8)
        if value == state:
            return
    raise AssertionError((point, state))


async def run(args):
    assert args.port != 25565, "never send bots to the online service"
    reference = json.loads(Path(args.reference).read_text())
    assert reference["protocol"] == 775 and reference["data_version"] == 4788
    assert hashlib.sha256(Path(args.blocks_report).read_bytes()).hexdigest() == reference["blocks_report_sha256"]
    state = report_states(args.blocks_report)
    ids = json.loads(Path(args.ids).read_text())
    items = {i["name"]: i["id"] for i in ids["items"]}
    settings = SimpleNamespace(host=args.host, port=args.port, timeout=35, run_id="P2A",
                               shared_zone=False, mode="connect", qa_verify_movement=False)
    a, b = Placer(0, settings), Placer(1, settings)
    a.name = "Ofwood_"
    rows = []
    try:
        await a.connect()
        await b.connect()
        await a.conn.send(44)
        await b.conn.send(44)
        if args.verify:
            snapshot = json.loads(Path(args.snapshot).read_text())
            rows = snapshot["states"]
            reverse = {v: k for k, v in state.items()}
            for index, row in enumerate(rows):
                if args.vanilla:
                    marker = f"P2A_PERSIST_{index}"
                    await command(a, f"execute if block {' '.join(map(str, row['point']))} {reverse[row['state']]} run tellraw Ofwood_ \"{marker}\"")
                    await until(lambda: any(marker.encode() in p for _, p in a.events[121]), marker)
                else:
                    assert await query_block(a, tuple(row["point"])) == row["state"], row
            assert latest_inventory(a)[36] == tuple(snapshot["slot36"])
            print(json.dumps({"status": "passed", "restart_states": len(rows), "slot36": snapshot["slot36"]}))
            return
        # Fixed fixture straddles negative chunk X and Z boundaries. Both actors
        # use server teleport commands, so Vanilla sees the same positions.
        await setblock(a, (-18, 63, 14), "stone")
        await setblock(a, (-20, 63, 14), "stone")
        await command(a, "tp -17.5 64 14.5")
        await command(a, f"tp {b.name} -19.5 64 14.5")
        await asyncio.sleep(0.5)
        cases = [
            ("stone", "stone", 1, (0.5, 1, 0.5), "air", "stone", False),
            ("dirt", "dirt", 1, (0.5, 1, 0.5), "air", "dirt", False),
            ("planks", "oak_planks", 1, (0.5, 1, 0.5), "air", "oak_planks", False),
            ("log-y", "oak_log", 1, (0.5, 1, 0.5), "air", "oak_log[axis=y]", False),
            ("log-x", "oak_log", 5, (1, 0.5, 0.5), "air", "oak_log[axis=x]", False),
            ("log-z", "oak_log", 3, (0.5, 0.5, 1), "air", "oak_log[axis=z]", False),
            ("slab-bottom", "oak_slab", 1, (0.5, 1, 0.5), "air", "oak_slab[type=bottom,waterlogged=false]", False),
            ("slab-top", "oak_slab", 0, (0.5, 0, 0.5), "air", "oak_slab[type=top,waterlogged=false]", False),
            ("merge-up", "oak_slab", 1, (0.5, 0.5, 0.5), "oak_slab[type=bottom,waterlogged=false]", "oak_slab[type=double,waterlogged=false]", True),
            ("merge-down", "oak_slab", 0, (0.5, 0.5, 0.5), "oak_slab[type=top,waterlogged=false]", "oak_slab[type=double,waterlogged=false]", True),
            ("merge-side", "oak_slab", 5, (1, 0.75, 0.5), "oak_slab[type=bottom,waterlogged=false]", "oak_slab[type=double,waterlogged=false]", True),
            ("no-merge-side", "oak_slab", 5, (1, 0.25, 0.5), "oak_slab[type=bottom,waterlogged=false]", "oak_slab[type=bottom,waterlogged=false]", False),
            ("replace-grass", "stone", 1, (0.5, 0.5, 0.5), "short_grass", "stone", True),
        ]
        a.sequence = 126  # exercise the 127/128 VarInt boundary against Vanilla
        point = (-16, 64, 16)
        for name, item, face, hit, initial, expected, replace in cases:
            await hold(a, items["minecraft:" + item])
            await command(a, "gamemode survival")
            # Fresh fixture for each action. Grass has real grass-block support.
            await setblock(a, (point[0], point[1]-1, point[2]), "grass_block[snowy=false]")
            for delta in ((0, 0, 0), (0, 1, 0), (1, 0, 0), (0, 0, 1)):
                await setblock(a, tuple(p+d for p, d in zip(point, delta)), "air")
            if initial != "air":
                await setblock(a, point, initial)
                clicked = point
            else:
                offsets = {0: (0, 1, 0), 1: (0, -1, 0), 3: (0, 0, -1), 5: (-1, 0, 0)}
                clicked = tuple(p+d for p, d in zip(point, offsets[face]))
                await setblock(a, clicked, "stone")
            target = point if replace or initial == "air" else (point[0]+1, point[1], point[2])
            for player in (a, b): player.clear_block_events(target)
            start = len(a.events[20])
            await place(a, clicked, face, hit)
            expected_id = state["minecraft:" + expected]
            await asyncio.gather(observed(a, target, expected_id), observed(b, target, expected_id))
            if args.vanilla:
                marker = "P2A_STATE_" + name
                await command(a, f"execute if block {' '.join(map(str,target))} {expected} run tellraw Ofwood_ \"{marker}\"")
                await until(lambda: any(marker.encode() in p for _, p in a.events[121]), marker)
            else:
                assert await query_block(a, target) == expected_id, name
            # Both servers emit a ContainerSetSlot on Survival consumption.
            await until(lambda: (36, items["minecraft:"+item], 3) in slot_updates_since(a, start), "Survival consumption")
            rows.append({"case": name, "point": list(target), "state": expected_id, "count": 3})
        # Invalid placements must preserve the world and Survival inventory.
        for name, target, clicked, face, hit in (
            ("occupied", point, (-16, 63, 16), 1, (0.5, 1, 0.5)),
            ("self-collision", (-18, 64, 14), (-18, 63, 14), 1, (0.5, 1, 0.5)),
            ("invalid-hit", point, (-16, 63, 16), 1, (0.5, 2.0, 0.5)),
            ("out-of-reach", (-8, 64, 16), (-8, 63, 16), 1, (0.5, 1, 0.5)),
        ):
            await hold(a, items["minecraft:stone"])
            await command(a, "gamemode survival")
            initial = "stone" if name == "occupied" else "air"
            await setblock(a, target, initial)
            for player in (a, b): player.clear_block_events(target)
            start = len(a.events[20])
            await place(a, clicked, face, hit)
            await asyncio.sleep(0.2)
            for player in (a, b):
                queue = player.block_events[target]
                while not queue.empty():
                    assert queue.get_nowait() == state["minecraft:"+initial], name
            assert all(count == 4 for slot, item, count in slot_updates_since(a, start) if slot == 36), name
            if args.vanilla:
                marker = "P2A_REJECT_" + name
                await command(a, f"execute if block {' '.join(map(str,target))} {initial} run tellraw Ofwood_ \"{marker}\"")
                await until(lambda: any(marker.encode() in p for _, p in a.events[121]), marker)
            else:
                assert await query_block(a, target) == state["minecraft:"+initial], name
            rows.append({"case": name, "point": list(target), "state": state["minecraft:"+initial], "count": 4})
        # A bottom slab does not occupy the top half of its voxel.
        await hold(a, items["minecraft:oak_slab"])
        await setblock(a, (-18, 64, 14), "air")
        await command(a, "tp -17.5 64.5 14.5")
        for player in (a, b): player.clear_block_events((-18, 64, 14))
        await place(a, (-18, 63, 14), 1, (0.5, 1, 0.5))
        slab = state["minecraft:oak_slab[type=bottom,waterlogged=false]"]
        await asyncio.gather(observed(a, (-18, 64, 14), slab), observed(b, (-18, 64, 14), slab))
        rows.append({"case": "partial-collision", "point": [-18,64,14], "state": slab, "count": 4})
        # A BlockEvent uses minecraft:block IDs, never the sorted descriptor index.
        await hold(a, items["minecraft:stone"])
        await setblock(a, point, "shulker_box[facing=up]")
        await setblock(a, (point[0], point[1]+1, point[2]), "air")
        start_events = len(a.events[7])
        await place(a, point, 1, (0.5, 1, 0.5))
        await until(lambda: len(a.events[7]) > start_events, "shulker BlockEvent")
        action = a.events[7][-1][1]
        protocol_id, end = read_varint(action, 10)
        assert action[8:10] == b"\x01\x01" and end == len(action)
        assert protocol_id == reference["shulker_block_protocol_id"]
        window, _ = read_varint(a.events[59][-1][1])
        await a.conn.send(19, varint(window))
        await asyncio.sleep(0.15)
        # Leave actual placements in both negative-coordinate chunks for restart.
        saved = []
        for target, item, key, face in (
            ((-16,64,15), "stone", "stone", 1),
            ((-16,64,16), "oak_log", "oak_log[axis=y]", 1),
            ((-15,64,15), "oak_slab", "oak_slab[type=bottom,waterlogged=false]", 1),
            ((-15,64,16), "oak_slab", "oak_slab[type=top,waterlogged=false]", 0),
        ):
            await hold(a, items["minecraft:"+item])
            await setblock(a, target, "air")
            clicked = (target[0], target[1] + (-1 if face == 1 else 1), target[2])
            await setblock(a, clicked, "stone")
            for player in (a, b): player.clear_block_events(target)
            await place(a, clicked, face, (0.5, 1 if face == 1 else 0, 0.5))
            expected_id = state["minecraft:"+key]
            await asyncio.gather(observed(a, target, expected_id), observed(b, target, expected_id))
            saved.append({"point": list(target), "state": expected_id})
        snapshot = {"states": saved, "slot36": [items["minecraft:oak_slab"],4]}
        assert rows == reference["scenarios"], "observations differ from captured Vanilla reference"
        assert snapshot == reference["after_restart"], "restart fixture differs from Vanilla reference"
        Path(args.snapshot).write_text(json.dumps(snapshot, indent=2))
        print(json.dumps({"status": "passed", "cases": rows, "observer": b.name, "shulker_block_protocol_id": protocol_id}))
    finally:
        await asyncio.gather(*(close(p) for p in (a, b) if p.conn and not p.conn.writer.is_closing()))


def slot_updates_since(bot, start):
    # Reuse the slot parser without changing the reader's event collection.
    return slot_updates(SimpleNamespace(events={20: bot.events[20][start:]}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--blocks-report", default="data/26.1.1/reports/blocks.json")
    parser.add_argument("--ids", default="src/generated/generated_minecraft_ids.json")
    parser.add_argument("--reference", default="tests/fixtures/vanilla-26.1.1-placement.json")
    parser.add_argument("--snapshot", required=True)
    parser.add_argument("--vanilla", action="store_true")
    parser.add_argument("--verify", action="store_true", help="GoatCore authoritative read after restart")
    asyncio.run(run(parser.parse_args()))
