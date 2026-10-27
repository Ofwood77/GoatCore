#!/usr/bin/env python3
"""Two-phase, remote-only player save/restart comparison on a disposable world."""

import argparse
import asyncio
import json
from pathlib import Path
import struct

from protocol import varint
from test_player_lifecycle import Player, latest_experience, latest_health, latest_inventory, settings
from test_player_visibility import close, offline_uuid, until


RICH = {9: (36, 32), 10: (939, 1), 36: (1, 64), 37: (937, 1),
        5: (971, 1), 45: (1296, 1)}


def updates_for(index, bots):
    if index == 1 and bots >= 2:
        return {**{slot: (1, (slot % 64) + 1) for slot in range(9, 45)},
                5: (971, 1), 6: (971, 1), 7: (971, 1), 8: (971, 1), 45: (1296, 1)}
    if index == 2 and bots >= 3:
        return {slot: (0, 0) for slot in range(5, 46)}
    return RICH


async def run(args):
    players = [Player(i, settings(args)) for i in range(args.bots)]
    snapshot_path = Path(args.snapshot)
    try:
        await asyncio.gather(*(player.connect() for player in players))
        await until(lambda: all(player.events[18] and player.events[104] and player.events[103]
                                for player in players), "initial player state", timeout=20)
        if args.phase == "prepare":
            rows = []
            for i, player in enumerate(players):
                assert player.gamemode == 1, (player.name, player.gamemode)
                slots = latest_inventory(player)
                updates = updates_for(i, args.bots)
                start_updates = len(player.events[20])
                for slot, (item, quantity) in updates.items():
                    payload = struct.pack(">h", slot) + varint(quantity)
                    if quantity:
                        payload += varint(item) + varint(0) + varint(0)
                    await player.conn.send(56, payload)
                    slots[slot] = (item, quantity)
                await until(lambda p=player, start=start_updates, count=len(updates):
                            len(p.events[20]) >= start + count, "creative slot acknowledgements", timeout=25)
                selected = (i + 2) % 9
                await player.conn.send(53, struct.pack(">h", selected))
                if i == 0:
                    await player.conn.send(40, b"\x02")  # Vanilla flying bit
                position = [-20.25 - i * 2.0, 64.0, 11.75 + i * 2.0]
                yaw = float((i * 37 + 45) % 360)
                pitch = float(i % 40 - 20)
                await player.conn.send(31, struct.pack(">dddff?", *position, yaw, pitch, True))
                rows.append({"name": player.name, "uuid": offline_uuid(player.name).hex(),
                             "slots": slots, "selected": selected, "position": position,
                             "flying": i == 0,
                             "gamemode": player.gamemode, "health": latest_health(player),
                             "experience": latest_experience(player)})
            await asyncio.sleep(0.5)
            snapshot_path.write_text(json.dumps(rows, indent=2))
            for player in players[:-1]:
                await close(player)
            players[-1].expected_close = True
            players[-1].conn.writer.transport.abort()
            await asyncio.sleep(0.5)
            for row in rows:
                value = row["uuid"]
                filename = f"{value[:8]}-{value[8:12]}-{value[12:16]}-{value[16:20]}-{value[20:]}" + ".dat"
                assert (Path(args.world_path) / "players" / "data" / filename).is_file(), row["name"]
            kinds = "rich" + (", full" if args.bots >= 2 else "") + (", empty" if args.bots >= 3 else "")
            print(f"PASS: saved {len(rows)} distinct players; {kinds} inventories; final client aborted")
        else:
            rows = json.loads(snapshot_path.read_text())
            assert len(rows) == len(players)
            for player, row in zip(players, rows):
                assert player.name == row["name"]
                assert player.gamemode == row["gamemode"], (player.name, player.gamemode)
                assert latest_inventory(player) == [tuple(slot) for slot in row["slots"]], player.name
                assert latest_health(player) == tuple(row["health"]), player.name
                assert latest_experience(player) == tuple(row["experience"]), player.name
                assert all(abs(a - b) < 0.02 for a, b in zip(player.pos, row["position"])), (player.name, player.pos)
                assert player.events[105] and player.events[105][-1][1][0] == row["selected"], player.name
                assert player.events[64] and bool(player.events[64][-1][1][0] & 0x02) == row["flying"], player.name
            print(f"PASS: {len(rows)} players restored exactly after restart, including 46 slots each")
    finally:
        await asyncio.gather(*(close(player) for player in players if player.conn and
                               not player.conn.writer.is_closing()), return_exceptions=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("phase", choices=("prepare", "verify"))
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--run-id", default="PE")
    parser.add_argument("--bots", type=int, choices=(1, 2, 10, 25), required=True)
    parser.add_argument("--world-path", required=True)
    parser.add_argument("--snapshot", required=True)
    args = parser.parse_args()
    asyncio.run(run(args))
