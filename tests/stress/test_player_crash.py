#!/usr/bin/env python3
"""Remote-only held client for SIGTERM/SIGKILL player durability checks."""

import argparse
import asyncio
import struct
from types import SimpleNamespace

from test_player_lifecycle import Player, command, latest_inventory
from test_player_visibility import close, until


async def run(args):
    settings = SimpleNamespace(host=args.host, port=args.port, timeout=30,
                               run_id="PCR", shared_zone=False, mode="connect",
                               qa_verify_movement=False)
    player = Player(0, settings)
    player.name = "Ofwood_"
    try:
        await player.connect()
        if args.phase == "hold":
            if args.give:
                await command(player, "qa give stone 1")
                await until(lambda: any(b"QA give item=minecraft:stone" in payload
                                        for _, payload in player.events[121]), "durable inventory update")
            _, y, z = player.pos
            await player.conn.send(31, struct.pack(">dddff?", args.x, y, z, 125.0, -15.0, True))
            await command(player, "qa pos")
            await until(lambda: any(f"x={args.x:.3f}".encode() in payload
                                    for _, payload in player.events[121]), "accepted movement")
            print(f"READY: connected, x={args.x:.3f}, stone={latest_inventory(player)[36]}", flush=True)
            await asyncio.sleep(120)
        else:
            assert abs(player.pos[0] - args.x) < 0.02, player.pos
            assert latest_inventory(player)[36] == (1, args.stone), latest_inventory(player)[36]
            print(f"PASS: persisted x={player.pos[0]:.3f}, stone={args.stone}")
    finally:
        if player.conn and not player.conn.writer.is_closing():
            await close(player)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("phase", choices=("hold", "check"))
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--x", type=float, required=True)
    parser.add_argument("--stone", type=int, default=0)
    parser.add_argument("--give", action="store_true")
    asyncio.run(run(parser.parse_args()))
