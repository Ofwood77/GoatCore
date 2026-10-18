#!/usr/bin/env python3
"""Exercise epoll output wakeups and connection lifetime against a running server."""

import argparse
import socket
import struct
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import mc_ping  # noqa: E402


def exercise(host: str, port: int, index: int) -> None:
    with socket.create_connection((host, port), timeout=5) as sock:
        sock.settimeout(5)
        if index % 4 == 0:
            sock.sendall(b"\x80")  # incomplete length VarInt, then disconnect
            return

        mc_ping.send_handshake(sock, host, port, next_state=1)
        mc_ping.send_packet(sock, 0, b"", compression_threshold=-1)
        if index % 4 == 1:
            return  # disconnect while the response is being prepared

        packet_id, payload = mc_ping.read_frame(sock, compression_threshold=-1)
        if packet_id != 0 or "GoatCore" not in mc_ping.read_string_payload(payload):
            raise AssertionError("invalid status response")

        ping = struct.pack(">q", index)
        mc_ping.send_packet(sock, 1, ping, compression_threshold=-1)
        packet_id, payload = mc_ping.read_frame(sock, compression_threshold=-1)
        if packet_id != 1 or payload != ping:
            raise AssertionError("missing or reordered pong")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host")
    parser.add_argument("port", type=int)
    parser.add_argument("--clients", type=int, default=96)
    args = parser.parse_args()
    with ThreadPoolExecutor(max_workers=16) as pool:
        futures = [pool.submit(exercise, args.host, args.port, i) for i in range(args.clients)]
        for future in as_completed(futures):
            future.result()
    print(f"network epoll: {args.clients} concurrent/repeated clients ok")


if __name__ == "__main__":
    main()
