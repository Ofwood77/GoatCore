#!/usr/bin/env python3
"""Protocol 775 checks of another player's entity lifecycle on a remote server.

Run on the dedicated test host only, against an offline disposable instance.
"""

import argparse
import asyncio
from collections import defaultdict
import hashlib
from types import SimpleNamespace
import struct
import time

from protocol import read_varint, varint
from stress import Bot


class Viewer(Bot):
    def __init__(self, index, args):
        super().__init__(index, args, lambda _: None)
        self.events = defaultdict(list)
        self.entity_id = None

    async def play_packet(self, packet_id, payload):
        if packet_id == 49:
            self.entity_id = struct.unpack_from(">i", payload)[0]
        if packet_id in (1, 2, 5, 53, 54, 56, 69, 70, 77, 83, 99, 102, 125):
            self.events[packet_id].append((time.monotonic(), payload))
        await super().play_packet(packet_id, payload)


def entity_ids(bot, packet_id):
    result = []
    for _, payload in bot.events[packet_id]:
        if packet_id == 77:
            count, offset = read_varint(payload)
            for _ in range(count):
                entity_id, offset = read_varint(payload, offset)
                result.append(entity_id)
        else:
            entity_id, offset = read_varint(payload)
            if packet_id == 1:
                # Add Entity also carries item drops and other entities.
                # The player type id in the 26.1.1 registry is 155.
                entity_type, _ = read_varint(payload, offset + 16)
                if entity_type != 155:
                    continue
            result.append(entity_id)
    return result


def offline_uuid(name):
    value = bytearray(hashlib.md5(("OfflinePlayer:" + name).encode("ascii")).digest())
    value[6] = (value[6] & 0x0F) | 0x30
    value[8] = (value[8] & 0x3F) | 0x80
    return bytes(value)


def has_profile(bot, packet_id, name):
    identity = offline_uuid(name)
    return any(identity in payload for _, payload in bot.events[packet_id])


def animations(bot, entity_id):
    values = []
    for _, payload in bot.events[2]:
        found, offset = read_varint(payload)
        if found == entity_id and len(payload) == offset + 1:
            values.append(payload[offset])
    return values


def base_flags(bot, entity_id):
    values = []
    for _, payload in bot.events[99]:
        found, offset = read_varint(payload)
        if found != entity_id or offset + 3 > len(payload) or payload[offset] != 0:
            continue
        serializer, offset = read_varint(payload, offset + 1)
        if serializer == 0 and offset < len(payload):
            values.append(payload[offset])
    return values


def poses(bot, entity_id):
    values = []
    for _, payload in bot.events[99]:
        found, offset = read_varint(payload)
        if found != entity_id or offset + 3 > len(payload) or payload[offset] != 6:
            continue
        serializer, offset = read_varint(payload, offset + 1)
        if serializer == 20:
            value, _ = read_varint(payload, offset)
            values.append(value)
    return values


async def until(predicate, label, timeout=12):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            return
        await asyncio.sleep(0.05)
    raise AssertionError(f"timed out waiting for {label}")


async def close(bot):
    bot.expected_close = True
    await bot.conn.close()
    if bot.reader_task:
        await asyncio.wait_for(bot.reader_task, 3)


async def main(args):
    settings = SimpleNamespace(host=args.host, port=args.port, timeout=25,
                               run_id=args.run_id, shared_zone=False, mode="connect",
                               qa_verify_movement=False)
    bots = []
    try:
        first = Viewer(0, settings)
        bots.append(first)
        await first.connect()
        second = Viewer(1, settings)
        bots.append(second)
        await second.connect()
        await until(lambda: len(entity_ids(first, 1)) >= 1 and
                    len(entity_ids(second, 1)) >= 1, "mutual entity spawn")
        first_id = entity_ids(second, 1)[0]
        second_id = entity_ids(first, 1)[0]
        assert first_id != second_id
        assert entity_ids(first, 1).count(second_id) == 1
        assert entity_ids(second, 1).count(first_id) == 1
        assert has_profile(first, 70, second.name) and has_profile(second, 70, first.name), "missing peer player info"

        x, y, z = second.pos
        await second.conn.send(31, struct.pack(">dddff?", x + 0.25, y, z, 90.0, 0.0, True))
        await until(lambda: any(second_id in entity_ids(first, kind)
                                for kind in (53, 54)) and
                    second_id in entity_ids(first, 83), "movement and head rotation")
        before = len(first.events[125])
        await second.conn.send(30, struct.pack(">ddd?", x + 9.25, y, z, True))
        await until(lambda: len(first.events[125]) > before, "large movement teleport")

        await close(second)
        await until(lambda: second_id in entity_ids(first, 77) and has_profile(first, 69, second.name),
                    "entity and player info removal")
        replacement = Viewer(1, settings)
        bots.append(replacement)
        await replacement.connect()
        await until(lambda: len(entity_ids(first, 1)) >= 2, "reconnect spawn")
        assert len(entity_ids(first, 1)) == 2, "duplicate entity on reconnect"
        assert replacement.pos is not None
        assert abs(replacement.pos[0] - (x + 9.25)) < 0.1, "position not restored"
        print("PASS: mutual spawn, relative movement, rotation, teleport, removal, reconnect, position")
    finally:
        for bot in bots:
            if bot.conn and not bot.conn.writer.is_closing():
                await close(bot)


async def fanout(args):
    settings = SimpleNamespace(host=args.host, port=args.port, timeout=45,
                               run_id=args.run_id, shared_zone=False, mode="connect",
                               qa_verify_movement=False)
    bots = [Viewer(index, settings) for index in range(args.fanout)]
    try:
        await asyncio.gather(*(bot.connect() for bot in bots))
        await until(lambda: all(len(entity_ids(bot, 1)) >= args.fanout - 1 for bot in bots),
                    f"{args.fanout}-player visibility", timeout=25)
        for bot in bots:
            ids = entity_ids(bot, 1)
            assert len(ids) == args.fanout - 1, (bot.name, "duplicate spawn", len(ids))
            assert len(set(ids)) == args.fanout - 1, (bot.name, "duplicate entity id")
            assert bot.error is None, (bot.name, bot.error)
        print(f"PASS: {args.fanout} players each received {args.fanout - 1} unique remote entities")
    finally:
        await asyncio.gather(*(close(bot) for bot in bots if bot.conn and
                               not bot.conn.writer.is_closing()), return_exceptions=True)


async def check_position(args):
    settings = SimpleNamespace(host=args.host, port=args.port, timeout=25,
                               run_id=args.run_id, shared_zone=False, mode="connect",
                               qa_verify_movement=False)
    bot = Viewer(1, settings)
    try:
        await bot.connect()
        assert abs(bot.pos[0] - args.check_position) < 0.1, (bot.pos, args.check_position)
        print(f"PASS: {bot.name} restored x={bot.pos[0]:.3f} after server restart")
    finally:
        if bot.conn and not bot.conn.writer.is_closing():
            await close(bot)


async def animation_checks(args):
    settings = SimpleNamespace(host=args.host, port=args.port, timeout=25,
                               run_id=args.run_id, shared_zone=False, mode="connect",
                               qa_verify_movement=False)
    watcher, actor = Viewer(0, settings), Viewer(1, settings)
    try:
        await watcher.connect()
        await actor.connect()
        await watcher.conn.send(44)
        await actor.conn.send(44)
        await until(lambda: actor.entity_id in entity_ids(watcher, 1), "actor spawn")
        await actor.conn.send(63, varint(0))
        await until(lambda: 0 in animations(watcher, actor.entity_id), "main-hand swing")
        await asyncio.sleep(0.7)
        await actor.conn.send(63, varint(1))
        await until(lambda: 3 in animations(watcher, actor.entity_id), "off-hand swing")
        await actor.conn.send(43, b"\x20")
        await until(lambda: any(flags & 0x02 for flags in base_flags(watcher, actor.entity_id)) and
                    5 in poses(watcher, actor.entity_id), "sneak metadata and pose")
        await actor.conn.send(43, b"\x00")
        await until(lambda: base_flags(watcher, actor.entity_id) and
                    not base_flags(watcher, actor.entity_id)[-1] & 0x02 and
                    poses(watcher, actor.entity_id)[-1] == 0, "stand metadata and pose")
        await actor.conn.send(42, varint(actor.entity_id) + varint(1) + varint(0))
        await until(lambda: any(flags & 0x08 for flags in base_flags(watcher, actor.entity_id)), "sprint metadata")
        await actor.conn.send(42, varint(actor.entity_id) + varint(2) + varint(0))
        await until(lambda: base_flags(watcher, actor.entity_id) and
                    not base_flags(watcher, actor.entity_id)[-1] & 0x08, "walk metadata")
        if not args.reference:
            await actor.conn.send(56, struct.pack(">h", 36) + varint(1) + varint(1) + varint(0) + varint(0))
            await until(lambda: len([p for _, p in watcher.events[102] if read_varint(p)[0] == actor.entity_id]) >= 2,
                        "visible main-hand equipment")
        print("PASS: arm swings, sneak, sprint" + ("" if args.reference else ", visible equipment"))
    finally:
        await asyncio.gather(*(close(bot) for bot in (watcher, actor) if bot.conn and
                               not bot.conn.writer.is_closing()), return_exceptions=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--run-id", default="P1C")
    parser.add_argument("--fanout", type=int, choices=(2, 5, 10, 25, 50))
    parser.add_argument("--check-position", type=float)
    parser.add_argument("--animations", action="store_true")
    parser.add_argument("--reference", action="store_true", help="skip creative equipment on Vanilla survival")
    options = parser.parse_args()
    if options.fanout and options.check_position is not None:
        parser.error("choose only one test mode")
    asyncio.run(animation_checks(options) if options.animations else
                check_position(options) if options.check_position is not None else
                fanout(options) if options.fanout else main(options))
