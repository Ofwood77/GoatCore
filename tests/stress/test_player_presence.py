#!/usr/bin/env python3
"""Check status population and Vanilla join/leave system chat on a remote server."""

import argparse
import asyncio
import json
from pathlib import Path
import socket
import sys
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import mc_ping  # noqa: E402

from test_player_visibility import Viewer, close, until


class PresenceViewer(Viewer):
    async def play_packet(self, packet_id, payload):
        if packet_id == 121:
            self.events[121].append((0, payload))
        await super().play_packet(packet_id, payload)


def online_players(host, port):
    with socket.create_connection((host, port), timeout=5) as sock:
        sock.settimeout(5)
        mc_ping.send_handshake(sock, host, port, next_state=1)
        mc_ping.send_packet(sock, 0, b"", compression_threshold=-1)
        packet_id, payload = mc_ping.read_frame(sock, compression_threshold=-1)
        assert packet_id == 0, packet_id
        data = json.loads(mc_ping.read_string_payload(payload))
        assert data["version"]["protocol"] == 775, data
        assert data["players"]["max"] >= data["players"]["online"], data
        return data["players"]["online"]


def presence_messages(bot, key, name):
    marker = key.encode("ascii")
    identity = name.encode("ascii")
    return [payload for _, payload in bot.events[121]
            if marker in payload and identity in payload and payload[-1:] == b"\0"]


async def run(args):
    settings = SimpleNamespace(host=args.host, port=args.port, timeout=30,
                               run_id=args.run_id, shared_zone=False,
                               mode="connect", qa_verify_movement=False)
    before = await asyncio.to_thread(online_players, args.host, args.port)
    first, second = PresenceViewer(0, settings), PresenceViewer(1, settings)
    try:
        await first.connect()
        await first.conn.send(44)
        await until(lambda: online_players(args.host, args.port) == before + 1,
                    "one online player")
        await second.connect()
        await second.conn.send(44)
        await until(lambda: online_players(args.host, args.port) == before + 2,
                    "two online players")
        await until(lambda: len(presence_messages(first, "multiplayer.player.joined", second.name)) == 1,
                    "join message")
        assert not presence_messages(second, "multiplayer.player.joined", second.name), "self join message"
        await close(second)
        await until(lambda: online_players(args.host, args.port) == before + 1,
                    "one online player after departure")
        await until(lambda: len(presence_messages(first, "multiplayer.player.left", second.name)) == 1,
                    "leave message")
        assert len(presence_messages(first, "multiplayer.player.joined", second.name)) == 1
        assert len(presence_messages(first, "multiplayer.player.left", second.name)) == 1
        print("PASS: status 0/1/2 transitions, one translated join and one translated leave")
    finally:
        await asyncio.gather(*(close(bot) for bot in (first, second) if bot.conn and
                               not bot.conn.writer.is_closing()), return_exceptions=True)
        await until(lambda: online_players(args.host, args.port) == before,
                    "initial player count restored")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--run-id", default="PRES")
    asyncio.run(run(parser.parse_args()))
