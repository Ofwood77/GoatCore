#!/usr/bin/env python3
"""Remote-only protocol checks for player persistence and concurrent departures."""

import argparse
import asyncio
import gzip
from pathlib import Path
import struct
from types import SimpleNamespace

from protocol import read_varint, string, varint
from test_player_visibility import Viewer, close, entity_ids, has_profile, offline_uuid, until


class Player(Viewer):
    async def play_packet(self, packet_id, payload):
        if packet_id in (18, 20, 38, 103, 104, 121):
            self.events[packet_id].append((asyncio.get_running_loop().time(), payload))
        await super().play_packet(packet_id, payload)


def settings(args):
    return SimpleNamespace(host=args.host, port=args.port, timeout=30,
                           run_id=args.run_id, shared_zone=False, mode="connect",
                           qa_verify_movement=False)


def latest_health(player):
    assert player.events[104], "no health packet"
    payload = player.events[104][-1][1]
    health = struct.unpack_from(">f", payload)[0]
    food, offset = read_varint(payload, 4)
    saturation = struct.unpack_from(">f", payload, offset)[0]
    return health, food, saturation


def latest_experience(player):
    assert player.events[103], "no experience packet"
    payload = player.events[103][-1][1]
    progress = struct.unpack_from(">f", payload)[0]
    level, offset = read_varint(payload, 4)
    total, _ = read_varint(payload, offset)
    return progress, level, total


def latest_inventory(player):
    for _, payload in reversed(player.events[18]):
        window, offset = read_varint(payload)
        if window != 0:
            continue
        _, offset = read_varint(payload, offset)
        count, offset = read_varint(payload, offset)
        assert count == 46, count
        slots = []
        for _ in range(count):
            quantity, offset = read_varint(payload, offset)
            if quantity == 0:
                slots.append((0, 0))
                continue
            item, offset = read_varint(payload, offset)
            added, offset = read_varint(payload, offset)
            removed, offset = read_varint(payload, offset)
            assert added == removed == 0, "test expects simple inventory slots"
            slots.append((item, quantity))
        return slots
    raise AssertionError("no player inventory packet")


def slot_updates(player):
    result = []
    for _, payload in player.events[20]:
        offset = 1  # window ID
        _, offset = read_varint(payload, offset)
        slot = struct.unpack_from(">h", payload, offset)[0]
        count, offset = read_varint(payload, offset + 2)
        item = read_varint(payload, offset)[0] if count else 0
        result.append((slot, item, count))
    return result


async def command(player, value):
    await player.conn.send(7, string(value))


async def persistence(args):
    player = Player(0, settings(args))
    player.name = "Ofwood_"
    try:
        await player.connect()
        await player.conn.send(44)
        assert has_profile(player, 70, player.name), "own offline UUID missing"
        if args.mode == "prepare":
            assert player.gamemode == 1, "prepare requires a disposable creative test player"
            for slot in (5, 36, 45):
                await player.conn.send(56, struct.pack(">h", slot) + varint(1) + varint(1) + varint(0) + varint(0))
            await until(lambda: all((i, 1, 1) in slot_updates(player) for i in (5, 36, 45)),
                        "inventory, armor and offhand updates")
            x, y, z = -17.25, 64.0, 15.75
            await player.conn.send(31, struct.pack(">dddff?", x, y, z, 135.0, -30.0, True))
            await command(player, "food set 13")
            await command(player, "food sat 3")
            await command(player, "health set 17")
            await command(player, "gamemode survival")
            await until(lambda: player.events[38] and player.events[38][-1][1][:1] == b"\x03" and
                        struct.unpack_from(">f", player.events[38][-1][1], 1)[0] == 0.0,
                        "survival mode")
            await until(lambda: latest_health(player) == (17.0, 13, 3.0), "health and food")
            assert all((i, 1, 1) in slot_updates(player) for i in (5, 36, 45))
            print("PASS: prepared UUID, position, rotation, gamemode, slots 5/36/45, health and food")
        else:
            assert player.gamemode == 0, player.gamemode
            assert all(abs(a - b) < 0.01 for a, b in zip(player.pos, (-17.25, 64.0, 15.75))), player.pos
            assert latest_health(player) == (17.0, 13, 3.0), latest_health(player)
            progress, level, total = latest_experience(player)
            assert abs(progress - 0.4) < 1e-6 and (level, total) == (7, 120), (progress, level, total)
            assert all(latest_inventory(player)[i] == (1, 1) for i in (5, 36, 45))
            await command(player, "qa pos")
            await until(lambda: any(b"yaw=135.00 pitch=-30.00" in payload for _, payload in player.events[121]),
                        "restored yaw and pitch")
            print("PASS: restored UUID, position, rotation, gamemode, inventory, armor, offhand, health, food and experience")
    finally:
        if player.conn and not player.conn.writer.is_closing():
            await close(player)


async def disconnects(args):
    cfg = settings(args)
    bots = [Player(i, cfg) for i in range(args.bots)]
    try:
        await asyncio.gather(*(bot.connect() for bot in bots))
        watcher = bots[0]
        await until(lambda: len(entity_ids(watcher, 1)) >= len(bots) - 1, "group spawn")
        expected = set(entity_ids(watcher, 1))
        assert len(expected) == len(bots) - 1
        for bot in bots[1:]:
            x, y, z = bot.pos
            await bot.conn.send(30, struct.pack(">ddd?", x + 0.25, y, z, True))
        # Abort sockets while movement packets are still reaching the tick.
        for bot in bots[1:]:
            bot.expected_close = True
            bot.conn.writer.transport.abort()
        await until(lambda: expected <= set(entity_ids(watcher, 77)), "all departed entities", timeout=20)
        assert all(has_profile(watcher, 69, bot.name) for bot in bots[1:]), "missing profile removal"
        newcomer = Player(1, cfg)
        bots.append(newcomer)
        end = asyncio.get_running_loop().time() + 10
        while True:
            try:
                await newcomer.connect()
                break
            except (RuntimeError, TimeoutError, OSError):
                if newcomer.conn:
                    await newcomer.conn.close()
                if asyncio.get_running_loop().time() >= end:
                    raise
                newcomer = Player(1, cfg)
                bots.append(newcomer)
                await asyncio.sleep(0.1)
        await until(lambda: len(entity_ids(watcher, 1)) >= args.bots, "reconnect entity")
        assert len(entity_ids(watcher, 1)) == args.bots, "duplicate reconnect spawn"
        print(f"PASS: {args.bots - 1} concurrent abrupt disconnects, removals and immediate reconnect")
    finally:
        await asyncio.gather(*(close(bot) for bot in bots if bot.conn and
                               not bot.conn.writer.is_closing()), return_exceptions=True)


async def chunk_abort(args):
    cfg = settings(args)
    bots = [Player(i, cfg) for i in range(args.bots)]
    tasks = [asyncio.create_task(bot.connect()) for bot in bots]
    try:
        await asyncio.wait_for(asyncio.gather(*(bot.spawn.wait() for bot in bots)), 30)
        assert all(bot.conn and bot.spawn_at is not None for bot in bots), "not all players entered Play"
        chunks_at_abort = [bot.chunk_packets for bot in bots]
        for bot in bots:
            bot.expected_close = True
            bot.conn.writer.transport.abort()
        await asyncio.gather(*tasks, return_exceptions=True)
        assert max(chunks_at_abort) < 100, chunks_at_abort
        replacement = Player(0, cfg)
        bots.append(replacement)
        end = asyncio.get_running_loop().time() + 10
        while True:
            try:
                await replacement.connect()
                break
            except (RuntimeError, TimeoutError, OSError):
                if replacement.conn:
                    await replacement.conn.close()
                if asyncio.get_running_loop().time() >= end:
                    raise
                replacement = Player(0, cfg)
                bots.append(replacement)
                await asyncio.sleep(0.1)
        print(f"PASS: {args.bots} aborts during initial chunk stream; immediate reconnect")
    finally:
        await asyncio.gather(*(close(bot) for bot in bots if bot.conn and
                               not bot.conn.writer.is_closing()), return_exceptions=True)


def seed_experience(args):
    assert args.world_path, "--world-path required"
    value = offline_uuid("Ofwood_").hex()
    filename = f"{value[:8]}-{value[8:12]}-{value[12:16]}-{value[16:20]}-{value[20:]}" + ".dat"
    path = Path(args.world_path) / "playerdata" / filename
    raw = gzip.decompress(path.read_bytes())
    for tag, payload in ((b"\x05\x00\x03XpP", struct.pack(">f", 0.4)),
                         (b"\x03\x00\x07XpLevel", struct.pack(">i", 7)),
                         (b"\x03\x00\x07XpTotal", struct.pack(">i", 120))):
        assert raw.count(tag) == 1, (path, tag)
        offset = raw.index(tag) + len(tag)
        raw = raw[:offset] + payload + raw[offset + 4:]
    replacement = path.with_suffix(".dat.tmp")
    replacement.write_bytes(gzip.compress(raw, mtime=0))
    replacement.replace(path)
    print("PASS: seeded disposable player NBT with 0.4 progress, level 7, total 120")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("prepare", "verify", "disconnects", "chunk-abort", "seed-xp"))
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--run-id", default="LC")
    parser.add_argument("--bots", type=int, default=10)
    parser.add_argument("--world-path")
    args = parser.parse_args()
    if args.mode == "seed-xp":
        seed_experience(args)
    else:
        asyncio.run(disconnects(args) if args.mode == "disconnects" else
                    chunk_abort(args) if args.mode == "chunk-abort" else persistence(args))
