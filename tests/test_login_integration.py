#!/usr/bin/env python3
"""Isolated protocol-775 login checks; never touches running instances."""

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


def offline_uuid(name):
    raw = bytearray(hashlib.md5(("OfflinePlayer:" + name).encode("ascii")).digest())
    raw[6] = (raw[6] & 0x0F) | 0x30
    raw[8] = (raw[8] & 0x3F) | 0x80
    return bytes(raw)


def login(port, name, forged_uuid=b"\xff" * 16):
    sock = socket.create_connection(("127.0.0.1", port), timeout=3)
    sock.settimeout(3)
    mc_ping.send_handshake(sock, "127.0.0.1", port, 2)
    mc_ping.send_packet(sock, 0, mc_ping.encode_string(name) + forged_uuid, -1)
    return sock


def expect_rejected(sock):
    try:
        packet_id, _ = mc_ping.read_frame(sock, -1)
        assert packet_id != 2, "invalid identity received Login Success"
    except (EOFError, ConnectionResetError, BrokenPipeError):
        pass
    finally:
        sock.close()


def main():
    with tempfile.TemporaryDirectory(prefix="goatcore-online-reject-") as config_dir:
        temp = Path(config_dir)
        (temp / "server.config").write_text("online-mode=true\n", encoding="ascii")
        (temp / "assets").symlink_to(ROOT / "assets", target_is_directory=True)
        online_port = free_port()
        online_env = os.environ.copy()
        online_env.update({"MC_SETUP_FILE": str(ROOT / "tests/stress/network/one.conf"),
                           "MC_BIND_PORT": str(online_port), "MC_WORLD_PATH": str(temp / "world")})
        online = subprocess.Popen([str(ROOT / "mc_server")], cwd=temp, env=online_env,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 8
            while True:
                try:
                    unauthenticated = login(online_port, "Notch")
                    break
                except OSError:
                    if online.poll() is not None or time.monotonic() >= deadline:
                        raise AssertionError("isolated online server did not start")
                    time.sleep(0.05)
            packet_id, payload = mc_ping.read_frame(unauthenticated, -1)
            assert packet_id == 1, "online login must request encryption"
            length, pos = mc_ping.decode_varint_from_bytes(payload, 0)
            assert length == 0, "server ID must be empty"
            key_len, pos = mc_ping.decode_varint_from_bytes(payload, pos)
            assert 200 <= key_len <= 4096
            pos += key_len
            token_len, pos = mc_ping.decode_varint_from_bytes(payload, pos)
            assert token_len == 16
            pos += token_len
            assert payload[pos:] == b"\x01", "Mojang authentication must be required"
            expect_rejected(login(online_port, "notch"))
            mc_ping.send_packet(unauthenticated, 1, b"\x00", -1)
            expect_rejected(unauthenticated)
        finally:
            online.terminate()
            try:
                online.wait(timeout=8)
            except subprocess.TimeoutExpired:
                online.kill()
                online.wait()

    port = free_port()
    with tempfile.TemporaryDirectory(prefix="goatcore-login-") as world:
        offline_temp = Path(world)
        (offline_temp / "server.config").write_text("online-mode=false\n", encoding="ascii")
        (offline_temp / "assets").symlink_to(ROOT / "assets", target_is_directory=True)
        env = os.environ.copy()
        env.update({
            "MC_BIND_PORT": str(port),
            "MC_WORLD_PATH": str(offline_temp / "world"),
            "MC_SETUP_FILE": str(ROOT / "tests/stress/network/one.conf"),
        })
        proc = subprocess.Popen([str(ROOT / "mc_server")], cwd=offline_temp, env=env,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        first = None
        try:
            deadline = time.monotonic() + 8
            while True:
                try:
                    first = login(port, "TestLogin")
                    break
                except (ConnectionRefusedError, OSError):
                    if proc.poll() is not None or time.monotonic() >= deadline:
                        raise AssertionError("isolated server did not start")
                    time.sleep(0.05)
            packet_id, payload = mc_ping.read_frame(first, -1)
            assert packet_id == 2 and payload[:16] == offline_uuid("TestLogin")

            expect_rejected(login(port, "TestLogin"))
            expect_rejected(login(port, "testlogin"))
            expect_rejected(login(port, "bad-name"))
            expect_rejected(login(port, "abcdefghijklmnopq"))
            first.close()
            first = None
            # The network owner removes the departed identity asynchronously.
            deadline = time.monotonic() + 3
            while True:
                second = login(port, "TestLogin")
                try:
                    packet_id, payload = mc_ping.read_frame(second, -1)
                    if packet_id == 2:
                        assert payload[:16] == offline_uuid("TestLogin")
                        break
                except (EOFError, ConnectionResetError):
                    pass
                finally:
                    second.close()
                if time.monotonic() >= deadline:
                    raise AssertionError("reconnect after disconnect rejected")
                time.sleep(0.05)
            assert mc_ping.run_login("127.0.0.1", port, "SelfInfoTest", 8) == 0
        finally:
            if first is not None:
                first.close()
            proc.terminate()
            try:
                proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
    print("login integration: offline UUID, duplicate, malformed, reconnect, self player info and online fail-closed ok")


if __name__ == "__main__":
    main()
