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
        if packet_id in (18, 20, 34, 38, 64, 68, 82, 103, 104, 105, 121):
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


async def death_cycle(args):
    admin = Player(0, settings(args))
    admin.name = "Ofwood_"
    peer = Player(1, settings(args))
    try:
        await admin.connect()
        await admin.conn.send(44)
        await peer.connect()
        await until(lambda: entity_ids(peer, 1), "peer sees admin")
        admin_entity = entity_ids(peer, 1)[0]
        if admin.gamemode != 1:
            await command(admin, "gamemode creative")
            await until(lambda: admin.events[38] and admin.events[38][-1][1][:1] == b"\x03" and
                        struct.unpack_from(">f", admin.events[38][-1][1], 1)[0] == 1.0,
                        "creative mode")
        await admin.conn.send(56, struct.pack(">h", 36) + varint(3) + varint(1) + varint(0) + varint(0))
        await until(lambda: (36, 1, 3) in slot_updates(admin), "stone stack")
        if args.expect_xp:
            assert latest_experience(admin)[1:] == (7, 120), latest_experience(admin)
        await command(admin, "gamemode survival")
        await until(lambda: admin.events[38] and admin.events[38][-1][1][:1] == b"\x03" and
                    struct.unpack_from(">f", admin.events[38][-1][1], 1)[0] == 0.0,
                    "survival mode")
        await command(admin, "health set 0")
        await until(lambda: admin.events[68] and latest_health(admin)[0] == 0.0,
                    "combat kill and zero health")
        await until(lambda: any(len(payload) == 5 and struct.unpack_from(">i", payload)[0] == admin_entity and
                                payload[4] == 3 for _, payload in peer.events[34]), "peer death event")
        expected_slot = (1, 3) if args.keep_inventory else (0, 0)
        assert latest_inventory(admin)[36] == expected_slot, "incorrect inventory at death"
        await admin.conn.send(12, varint(0))
        await until(lambda: admin.events[82] and latest_health(admin)[0] == 20.0,
                    "respawn and full health")
        assert latest_inventory(admin)[36] == expected_slot, "incorrect inventory after respawn"
        if not args.keep_inventory:
            assert latest_experience(admin) == (0.0, 0, 0), latest_experience(admin)
        print("PASS: death packet, peer event, respawn and expected keepInventory state")
    finally:
        await asyncio.gather(*(close(bot) for bot in (admin, peer) if bot.conn and
                               not bot.conn.writer.is_closing()), return_exceptions=True)


async def death_respawn_abort(args):
    admin = Player(0, settings(args))
    admin.name = "Ofwood_"
    peer = Player(1, settings(args))
    replacement = None
    try:
        await admin.connect()
        await admin.conn.send(44)
        await peer.connect()
        await until(lambda: entity_ids(peer, 1), "peer sees admin")
        old_entity = entity_ids(peer, 1)[0]
        if admin.gamemode != 1:
            await command(admin, "gamemode creative")
            await until(lambda: admin.events[38] and
                        struct.unpack_from(">f", admin.events[38][-1][1], 1)[0] == 1.0,
                        "creative mode")
        await admin.conn.send(56, struct.pack(">h", 36) + varint(2) + varint(1) + varint(0) + varint(0))
        await until(lambda: (36, 1, 2) in slot_updates(admin), "stone stack")
        await command(admin, "gamemode survival")
        await until(lambda: admin.events[38] and
                    struct.unpack_from(">f", admin.events[38][-1][1], 1)[0] == 0.0,
                    "survival mode")
        await command(admin, "health set 0")
        await until(lambda: admin.events[68] and latest_health(admin)[0] == 0.0,
                    "combat kill")
        await admin.conn.send(12, varint(0))
        admin.expected_close = True
        admin.conn.writer.transport.abort()
        await until(lambda: old_entity in entity_ids(peer, 77), "dead player removal")
        replacement = Player(0, settings(args))
        replacement.name = "Ofwood_"
        await replacement.connect()
        await replacement.conn.send(44)
        await until(lambda: len(entity_ids(peer, 1)) >= 2, "respawn reconnect visible")
        assert latest_health(replacement)[0] == 20.0, latest_health(replacement)
        assert latest_inventory(replacement)[36] == (0, 0), "duplicated inventory after abort"
        assert latest_experience(replacement) == (0.0, 0, 0), latest_experience(replacement)
        print("PASS: abort during respawn, peer removal/reappearance, healthy reconnect and no duplicated inventory or XP")
    finally:
        await asyncio.gather(*(close(bot) for bot in (admin, peer, replacement) if bot and bot.conn and
                               not bot.conn.writer.is_closing()), return_exceptions=True)


def seed_experience(args):
    assert args.world_path, "--world-path required"
    value = offline_uuid("Ofwood_").hex()
    filename = f"{value[:8]}-{value[8:12]}-{value[12:16]}-{value[16:20]}-{value[20:]}" + ".dat"
    path = Path(args.world_path) / "players" / "data" / filename
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
    parser.add_argument("mode", choices=("prepare", "verify", "disconnects", "chunk-abort", "seed-xp", "death", "death-abort"))
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--run-id", default="LC")
    parser.add_argument("--bots", type=int, default=10)
    parser.add_argument("--world-path")
    parser.add_argument("--expect-xp", action="store_true")
    parser.add_argument("--keep-inventory", action="store_true")
    args = parser.parse_args()
    if args.mode == "seed-xp":
        seed_experience(args)
    else:
        asyncio.run(disconnects(args) if args.mode == "disconnects" else
                    chunk_abort(args) if args.mode == "chunk-abort" else
                    death_cycle(args) if args.mode == "death" else
                    death_respawn_abort(args) if args.mode == "death-abort" else persistence(args))
