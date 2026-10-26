#!/usr/bin/env python3
"""Run on the remote host against isolated 26.1.1 Vanilla or GoatCore worlds."""

import argparse
import asyncio
from collections import defaultdict
import math
import struct
import time
from types import SimpleNamespace

from protocol import read_varint, string, varint
from test_player_visibility import Viewer, close, entity_ids, until


class CombatPlayer(Viewer):
    def __init__(self, index, settings):
        super().__init__(index, settings)
        self.combat_events = defaultdict(list)

    async def play_packet(self, packet_id, payload):
        if packet_id in (34, 68, 101, 104):
            self.combat_events[packet_id].append((time.monotonic(), payload))
        await super().play_packet(packet_id, payload)


def health(player):
    return struct.unpack_from(">f", player.combat_events[104][-1][1])[0]


def motion(player, entity_id, since=0.0):
    for timestamp, payload in reversed(player.combat_events[101]):
        if timestamp < since:
            continue
        found, offset = read_varint(payload)
        if found != entity_id or len(payload) < offset + 6:
            continue
        first, second = payload[offset:offset + 2]
        if first == 0:
            return (0.0, 0.0, 0.0)
        bits = (first | second << 8 | struct.unpack_from(">I", payload, offset + 2)[0] << 16)
        scale = first & 3
        if first & 4:
            extra, _ = read_varint(payload, offset + 6)
            scale |= (extra & 0xffffffff) << 2
        assert scale > 0
        return tuple((min((bits >> shift) & 32767, 32766) * 2 / 32766 - 1) * scale
                     for shift in (3, 18, 33))
    return None


async def move_to(player, destination):
    start = player.pos
    steps = max(1, math.ceil(max(abs(a - b) for a, b in zip(start, destination))))
    for step in range(1, steps + 1):
        point = tuple(a + (b - a) * step / steps for a, b in zip(start, destination))
        await player.conn.send(30, struct.pack(">ddd?", *point, True))
        await asyncio.sleep(0.06)


async def void_creative(args):
    settings = SimpleNamespace(host=args.host, port=args.port, timeout=35,
                               run_id=args.run_id, shared_zone=False, mode="connect",
                               qa_verify_movement=False)
    player = CombatPlayer(0, settings)
    try:
        await player.connect()
        await player.conn.send(44)
        assert player.gamemode == 1, "void test requires a fresh Creative GoatCore player"
        x, _, z = player.pos
        await player.conn.send(30, struct.pack(">ddd?", x, -129.0, z, False))
        await until(lambda: any(struct.unpack_from(">f", payload)[0] <= 16.0
                                for _, payload in player.combat_events[104]), "Creative void damage")
        values = [struct.unpack_from(">f", payload)[0] for _, payload in player.combat_events[104]]
        assert 16.0 in values, values
        print(f"PASS port={args.port} Creative void: 20 -> 16 health")
    finally:
        if player.conn and not player.conn.writer.is_closing():
            await close(player)


async def main(args):
    settings = SimpleNamespace(host=args.host, port=args.port, timeout=35,
                               run_id=args.run_id, shared_zone=False, mode="connect",
                               qa_verify_movement=False)
    attacker = CombatPlayer(0, settings)
    target = CombatPlayer(1, settings)
    replacement = None
    try:
        await attacker.connect()
        await target.connect()
        await attacker.conn.send(44)
        await target.conn.send(44)
        if args.goatcore:
            await attacker.conn.send(7, string("gamemode survival"))
            await target.conn.send(7, string("gamemode survival"))
            await asyncio.sleep(0.2)
        await until(lambda: target.entity_id in entity_ids(attacker, 1), "target spawn")
        x, y, z = attacker.pos
        await attacker.conn.send(30, struct.pack(">ddd?", x, y, z, True))
        await move_to(target, (x, y, z + args.direction * args.distance))
        await asyncio.sleep(1.0)
        initial = health(target)
        packet = varint(target.entity_id)
        attacked_at = time.monotonic()
        await attacker.conn.send(1, packet)
        await until(lambda: health(target) < initial, "first PvP damage")
        first = health(target)
        await until(lambda: motion(target, target.entity_id, attacked_at) is not None, "target motion")
        vx, vy, vz = motion(target, target.entity_id, attacked_at)
        observer_motion = motion(attacker, target.entity_id, attacked_at)
        assert first == initial - 1.0, (initial, first)
        assert abs(vx) < 0.06 and vz * args.direction > 0.30 and vy >= 0.0, (vx, vy, vz)
        await attacker.conn.send(1, packet)
        await asyncio.sleep(0.2)
        assert health(target) == first, (first, health(target))
        if args.goatcore:
            await close(target)
            await until(lambda: target.entity_id in entity_ids(attacker, 77), "combat departure")
            replacement = CombatPlayer(1, settings)
            await replacement.connect()
            await until(lambda: replacement.entity_id in entity_ids(attacker, 1), "combat reconnect")
            assert replacement.entity_id != target.entity_id
            assert health(replacement) == first, (first, health(replacement))
        print(f"PASS port={args.port} distance={args.distance} direction={args.direction:+d} "
              f"unarmed={initial - first:.1f} hp, "
              f"motion=({vx:.3f},{vy:.3f},{vz:.3f}), "
              f"observer_motion={observer_motion}, immediate second hit ignored" +
              (", combat departure/reconnect clean" if args.goatcore else ""))
    finally:
        await asyncio.gather(*(close(bot) for bot in (attacker, target, replacement) if bot and bot.conn and
                               not bot.conn.writer.is_closing()), return_exceptions=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--run-id", default="CB")
    parser.add_argument("--distance", type=float, default=2.0)
    parser.add_argument("--direction", type=int, choices=(-1, 1), default=1)
    parser.add_argument("--goatcore", action="store_true", help="set both players to survival on an isolated admin-enabled GoatCore stage")
    parser.add_argument("--void-creative", action="store_true")
    options = parser.parse_args()
    asyncio.run(void_creative(options) if options.void_creative else main(options))
