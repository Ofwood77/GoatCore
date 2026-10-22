#!/usr/bin/env python3
"""Opt-in live Mojang lookup; intentionally not part of precommit-check."""

import base64
import hashlib
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import mc_ping  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def read_string(data, pos):
    length, pos = mc_ping.decode_varint_from_bytes(data, pos)
    if length < 0 or pos + length > len(data):
        raise ValueError("malformed profile string")
    return data[pos:pos + length].decode("utf-8"), pos + length


def run():
    port = free_port()
    with tempfile.TemporaryDirectory(prefix="goatcore-skin-live-") as tmp:
        temp = Path(tmp)
        (temp / "assets").symlink_to(ROOT / "assets", target_is_directory=True)
        (temp / "server.config").write_text("online-mode=false\noffline-skins=true\n", encoding="ascii")
        env = os.environ.copy()
        env.update({"MC_BIND_PORT": str(port), "MC_WORLD_PATH": str(temp / "world"),
                    "MC_SETUP_FILE": str(ROOT / "tests/stress/network/one.conf")})
        with (temp / "server.log").open("w", encoding="utf-8") as log:
            server = subprocess.Popen([str(ROOT / "mc_server")], cwd=temp, env=env,
                                      stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 8
                while True:
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                            break
                    except OSError:
                        if server.poll() is not None or time.monotonic() >= deadline:
                            raise RuntimeError("server did not start")
                        time.sleep(0.05)
                with socket.create_connection(("127.0.0.1", port), timeout=8) as sock:
                    sock.settimeout(8)
                    mc_ping.send_handshake(sock, "127.0.0.1", port, 2)
                    mc_ping.send_packet(sock, 0, mc_ping.encode_string("Notch") + b"\0" * 16, -1)
                    packet_id, payload = mc_ping.read_frame(sock, -1)
                    assert packet_id == 2, f"unexpected login packet {packet_id}"
                    offline_uuid = bytearray(hashlib.md5(b"OfflinePlayer:Notch").digest())
                    offline_uuid[6] = (offline_uuid[6] & 15) | 0x30
                    offline_uuid[8] = (offline_uuid[8] & 63) | 0x80
                    assert payload[:16] == offline_uuid
                    name, pos = read_string(payload, 16)
                    assert name == "Notch"
                    count, pos = mc_ping.decode_varint_from_bytes(payload, pos)
                    assert count == 1, f"expected signed texture property, got {count}"
                    key, pos = read_string(payload, pos)
                    value, pos = read_string(payload, pos)
                    assert payload[pos] == 1
                    signature, pos = read_string(payload, pos + 1)
                    assert key == "textures" and pos == len(payload)
                    assert base64.b64decode(value, validate=True)
                    assert base64.b64decode(signature, validate=True)
                    print("live offline skin: signed Mojang textures in Login Success; UUID remains offline")
            finally:
                server.terminate()
                try:
                    server.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()
                if server.returncode not in (0, -15):
                    print((temp / "server.log").read_text(encoding="utf-8", errors="replace")[-3000:])


if __name__ == "__main__":
    if os.environ.get("RUN_MOJANG_LIVE_TESTS") != "1":
        raise SystemExit("set RUN_MOJANG_LIVE_TESTS=1 to query Mojang")
    run()
