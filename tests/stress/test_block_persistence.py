#!/usr/bin/env python3
"""Verify bot block edits survive clean shutdown and restart on a disposable world."""

import argparse
import asyncio
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import secrets
import signal
import socket
import struct
import subprocess
import time

from protocol import position, string, varint
from stress import Bot, QA_BLOCK, block_target_offset, options


ROOT = Path(__file__).resolve().parents[2]


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


async def start_server(binary, world, port, log_path):
    env = os.environ.copy()
    env.update({"MC_SETUP_FILE": str(ROOT / "tests/stress/network/one.conf"),
                "MC_WORLD_PATH": str(world), "MC_BIND_PORT": str(port), "MC_QA_MODE": "1",
                "MC_ADMIN_COMMANDS": "1"})
    log_file = log_path.open("w", encoding="utf-8")
    proc = subprocess.Popen([str(binary)], cwd=ROOT, env=env, stdout=log_file,
                            stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                raise RuntimeError(f"server exited before listening: {proc.returncode}")
            try:
                reader, writer = await asyncio.open_connection("127.0.0.1", port)
                writer.close()
                await writer.wait_closed()
                return proc, log_file
            except ConnectionRefusedError:
                await asyncio.sleep(0.05)
        raise RuntimeError("server did not listen within 10 seconds")
    except BaseException:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            proc.wait(timeout=10)
        log_file.close()
        raise


async def stop_server(proc, log_file):
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
    try:
        try:
            await asyncio.to_thread(proc.wait, 10)
        except subprocess.TimeoutExpired:
            proc.kill()
            await asyncio.to_thread(proc.wait, 10)
            raise RuntimeError("server did not stop cleanly; killed temporary instance")
    finally:
        log_file.close()
    if proc.returncode != 0:
        raise RuntimeError(f"server exited with {proc.returncode}")


async def query_block(bot, block):
    while not bot.qa_messages.empty():
        bot.qa_messages.get_nowait()
    deadline = time.monotonic() + bot.args.timeout
    while time.monotonic() < deadline:
        await bot.conn.send(9, string(f"!qa block {block[0]} {block[1]} {block[2]}"))
        message = await asyncio.wait_for(bot.qa_messages.get(), max(0.001, deadline - time.monotonic()))
        match = QA_BLOCK.search(message)
        if match:
            result = tuple(int(x) for x in match.groups())
            if result[:3] != block:
                raise AssertionError(f"QA returned wrong coordinates: {result} != {block}")
            return result[3]
        if b"QA block: unavailable" not in message:
            raise AssertionError(f"QA block query failed: {message[:240]!r}")
        await asyncio.sleep(0.05)
    raise TimeoutError(f"chunk never became ready at {block}")


async def close_bot(bot):
    if bot.conn:
        bot.expected_close = True
        await bot.conn.close()
    if bot.reader_task:
        await asyncio.wait_for(bot.reader_task, 2)


async def place_block(bot, target):
    bot.clear_block_events(target)
    bot.sequence += 1
    base = (target[0], target[1] - 1, target[2])
    payload = varint(0) + position(*base) + varint(1) + struct.pack(">fff?", 0.5, 1.0, 0.5, False) + varint(bot.sequence)
    await bot.conn.send(66, payload)
    if await bot.expect_block(target, lambda state: state != 0) is None:
        raise AssertionError(f"placement not confirmed at {target}")
    observed = await query_block(bot, target)
    if observed <= 0:
        raise AssertionError(f"placed block state is {observed} at {target}")
    return observed


async def break_block(bot, target):
    bot.clear_block_events(target)
    bot.sequence += 1
    await bot.conn.send(41, varint(0) + position(*target) + varint(1) + varint(bot.sequence))
    if await bot.expect_block(target, lambda state: state == 0) is None:
        raise AssertionError(f"break not confirmed at {target}")
    if await query_block(bot, target) != 0:
        raise AssertionError(f"broken block is not air at {target}")


async def bot_stage(args, phase, targets=None, expected=None):
    args.run_id = secrets.token_hex(2)
    args.place_limiter = None
    args.break_limiter = None
    bots = [Bot(i, args, lambda message: None) for i in range(3 if phase == "edit" else 1)]
    try:
        await asyncio.gather(*(bot.connect() for bot in bots))
        await asyncio.gather(*(asyncio.wait_for(bot.chunks.wait(), args.timeout) for bot in bots))
        if phase == "edit":
            targets = []
            for bot in bots:
                ox, oz = block_target_offset(bot.index)
                targets.append((math.floor(bot.pos[0]) + ox, math.floor(bot.pos[1]),
                                math.floor(bot.pos[2]) + oz))
            await asyncio.gather(*(query_block(bot, target) for bot, target in zip(bots, targets)))
            await asyncio.gather(*(bot.setup_block_item() for bot in bots))
            cycles = []
            for bot, target in zip(bots, targets):
                cycles.append(await bot.block_cycle(target))
            if not all(cycles):
                raise AssertionError(f"initial place+break cycles failed: {cycles}, "
                                     f"bot states={[(bot.placed, bot.broken, bot.error) for bot in bots]}")
            expected = [0, 0, 0]
            for index in (0, 2):
                expected[index] = await place_block(bots[index], targets[index])
            return targets, expected
        bot = bots[0]
        if targets is None or expected is None:
            raise AssertionError("restart stage needs expected targets")
        for target, state in zip(targets, expected):
            observed = await query_block(bot, target)
            if observed != state:
                raise AssertionError(f"{phase}: persisted state {observed} at {target}, expected {state}")
        if phase == "break":
            await break_block(bot, targets[0])
            expected = [0, expected[1], expected[2]]
        return targets, expected
    finally:
        await asyncio.gather(*(close_bot(bot) for bot in bots))


async def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server-binary", type=Path, default=ROOT / "mc_server")
    cli = parser.parse_args()
    binary = cli.server_binary.resolve()
    if not binary.is_file():
        parser.error(f"server binary not found: {binary}")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    result_dir = ROOT / "tests/stress/results" / f"block-persistence-{stamp}"
    result_dir.mkdir(parents=True)
    world = result_dir / "world"
    world.mkdir()
    targets = None
    expected = None
    first_restart = None
    for phase in ("edit", "break", "verify_final"):
        port = free_port()
        args = options(["--mode", "blocks", "--bots", "3", "--allow-block-changes",
                        "--host", "127.0.0.1", "--port", str(port), "--timeout", "60",
                        "--block-cooldown", "0"])
        proc, log_file = await start_server(binary, world, port, result_dir / f"server-{phase}.log")
        try:
            targets, expected = await bot_stage(args, phase, targets, expected)
            if phase == "edit":
                first_restart = expected.copy()
        finally:
            await stop_server(proc, log_file)
    result = {"targets": targets, "after_first_restart": first_restart,
              "after_second_restart": expected, "bots_editing": 3,
              "world": str(world), "status": "passed"}
    (result_dir / "report.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result))


if __name__ == "__main__":
    asyncio.run(main())
