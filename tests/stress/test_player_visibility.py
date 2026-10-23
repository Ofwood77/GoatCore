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

from protocol import read_varint
from stress import Bot


class Viewer(Bot):
    def __init__(self, index, args):
        super().__init__(index, args, lambda _: None)
        self.events = defaultdict(list)

    async def play_packet(self, packet_id, payload):
        if packet_id in (1, 53, 54, 56, 69, 70, 77, 83, 125):
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
            entity_id, _ = read_varint(payload)
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


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--run-id", default="P1C")
    parser.add_argument("--fanout", type=int, choices=(2, 5, 10, 25, 50))
    parser.add_argument("--check-position", type=float)
    options = parser.parse_args()
    if options.fanout and options.check_position is not None:
        parser.error("choose only one test mode")
    asyncio.run(check_position(options) if options.check_position is not None else
                fanout(options) if options.fanout else main(options))
