#!/usr/bin/env python3
"""Opt-in real hasJoined denial: no account session is forged or bypassed."""

import os
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import tempfile
import time

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import padding

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import mc_ping  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def read_blob(data, pos):
    length, pos = mc_ping.decode_varint_from_bytes(data, pos)
    assert 0 <= length <= 4096 and pos + length <= len(data)
    return data[pos:pos + length], pos + length


def run():
    port = free_port()
    with tempfile.TemporaryDirectory(prefix="goatcore-online-live-") as tmp:
        temp = Path(tmp)
        (temp / "server.config").write_text("online-mode=true\n", encoding="ascii")
        (temp / "assets").symlink_to(ROOT / "assets", target_is_directory=True)
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
                        sock = socket.create_connection(("127.0.0.1", port), timeout=0.2)
                        break
                    except OSError:
                        if server.poll() is not None or time.monotonic() >= deadline:
                            raise RuntimeError("online test server did not start")
                        time.sleep(0.05)
                with sock:
                    sock.settimeout(8)
                    mc_ping.send_handshake(sock, "127.0.0.1", port, 2)
                    mc_ping.send_packet(sock, 0, mc_ping.encode_string("Notch") + bytes(16), -1)
                    packet_id, payload = mc_ping.read_frame(sock, -1)
                    assert packet_id == 1
                    server_id, pos = read_blob(payload, 0)
                    public_der, pos = read_blob(payload, pos)
                    token, pos = read_blob(payload, pos)
                    assert not server_id and len(token) == 16 and payload[pos:] == b"\x01"
                    public_key = serialization.load_der_public_key(public_der)
                    secret = secrets.token_bytes(16)
                    encrypted_secret = public_key.encrypt(secret, padding.PKCS1v15())
                    encrypted_token = public_key.encrypt(token, padding.PKCS1v15())
                    response = (mc_ping.encode_varint(len(encrypted_secret)) + encrypted_secret +
                                mc_ping.encode_varint(len(encrypted_token)) + encrypted_token)
                    mc_ping.send_packet(sock, 1, response, -1)
                    # We deliberately did not call Mojang joinServer. A correct
                    # server must reject this otherwise-valid RSA exchange.
                    assert sock.recv(1) == b"", "unjoined account was accepted"
                assert server.poll() is None, "server crashed after rejected session"
            finally:
                server.terminate()
                try:
                    server.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()
            assert server.returncode in (0, -15), (temp / "server.log").read_text()[-3000:]
            log_text = (temp / "server.log").read_text(encoding="utf-8", errors="replace")
            assert "online crypto established: name=Notch" in log_text, log_text[-3000:]
            assert "online session rejected: name=Notch" in log_text, log_text[-3000:]
    print("online live: RSA exchange accepted, Mojang hasJoined denial enforced")


if __name__ == "__main__":
    if os.environ.get("RUN_MOJANG_LIVE_TESTS") != "1":
        raise SystemExit("set RUN_MOJANG_LIVE_TESTS=1 to query Mojang")
    run()
