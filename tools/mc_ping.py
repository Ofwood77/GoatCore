#!/usr/bin/env python3
import argparse
import socket
import struct
import sys
import time
import zlib


MC_PROTO_VERSION_1_21_1 = 767


def encode_varint(value: int) -> bytes:
    value &= 0xFFFFFFFF
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            break
    return bytes(out)


def decode_varint_from_bytes(data: bytes, offset: int) -> tuple[int, int]:
    result = 0
    shift = 0
    for i in range(5):
        if offset + i >= len(data):
            raise ValueError("varint truncated")
        byte = data[offset + i]
        result |= (byte & 0x7F) << shift
        if (byte & 0x80) == 0:
            # signed 32-bit
            if result & (1 << 31):
                result -= 1 << 32
            return result, offset + i + 1
        shift += 7
    raise ValueError("varint too long")


def read_exact(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("connection closed")
        buf += chunk
    return bytes(buf)


def read_varint(sock: socket.socket) -> int:
    result = 0
    shift = 0
    for _ in range(5):
        b = sock.recv(1)
        if not b:
            raise EOFError("connection closed")
        byte = b[0]
        result |= (byte & 0x7F) << shift
        if (byte & 0x80) == 0:
            if result & (1 << 31):
                result -= 1 << 32
            return result
        shift += 7
    raise ValueError("varint too long")


def encode_string(s: str) -> bytes:
    b = s.encode("utf-8")
    return encode_varint(len(b)) + b


def read_string_payload(payload: bytes) -> str:
    slen, pos = decode_varint_from_bytes(payload, 0)
    if slen < 0:
        raise ValueError("negative string length")
    end = pos + slen
    if end > len(payload):
        raise ValueError("string truncated")
    return payload[pos:end].decode("utf-8", errors="replace")


def send_packet(sock: socket.socket, packet_id: int, payload: bytes, compression_threshold: int) -> None:
    body = encode_varint(packet_id) + payload

    if compression_threshold >= 0:
        if compression_threshold > 0 and len(body) >= compression_threshold:
            comp = zlib.compress(body)
            packet_data = encode_varint(len(body)) + comp
        else:
            packet_data = encode_varint(0) + body
    else:
        packet_data = body

    sock.sendall(encode_varint(len(packet_data)) + packet_data)


def read_frame(sock: socket.socket, compression_threshold: int) -> tuple[int, bytes]:
    packet_len = read_varint(sock)
    if packet_len <= 0:
        raise ValueError(f"invalid packet length {packet_len}")

    raw = read_exact(sock, packet_len)

    if compression_threshold >= 0:
        data_len, pos = decode_varint_from_bytes(raw, 0)
        if data_len == 0:
            uncompressed = raw[pos:]
        else:
            uncompressed = zlib.decompress(raw[pos:])
            if len(uncompressed) != data_len:
                raise ValueError(f"bad decompressed length: got={len(uncompressed)} want={data_len}")
    else:
        uncompressed = raw

    packet_id, pos = decode_varint_from_bytes(uncompressed, 0)
    return packet_id, uncompressed[pos:]


def send_handshake(sock: socket.socket, host: str, port: int, next_state: int) -> None:
    payload = (
        encode_varint(MC_PROTO_VERSION_1_21_1)
        + encode_string(host)
        + struct.pack(">H", port)
        + encode_varint(next_state)
    )
    send_packet(sock, 0x00, payload, compression_threshold=-1)


def run_status(host: str, port: int, timeout_s: float) -> int:
    sock = socket.create_connection((host, port), timeout=timeout_s)
    sock.settimeout(timeout_s)
    try:
        send_handshake(sock, host, port, next_state=1)
        send_packet(sock, 0x00, b"", compression_threshold=-1)  # Status Request

        pid, payload = read_frame(sock, compression_threshold=-1)
        if pid != 0x00:
            raise ValueError(f"expected status response 0x00, got 0x{pid:02X}")
        status_json = read_string_payload(payload)
        print(status_json)

        ping_payload = struct.pack(">q", int(time.time() * 1000))
        send_packet(sock, 0x01, ping_payload, compression_threshold=-1)

        pid, payload = read_frame(sock, compression_threshold=-1)
        if pid != 0x01 or len(payload) != 8:
            raise ValueError(f"expected pong 0x01 len=8, got 0x{pid:02X} len={len(payload)}")
        if payload != ping_payload:
            raise ValueError("pong payload mismatch")
        print("pong ok")
        return 0
    finally:
        sock.close()


def run_login(host: str, port: int, username: str, timeout_s: float) -> int:
    sock = socket.create_connection((host, port), timeout=timeout_s)
    sock.settimeout(0.5)
    compression_threshold = -1

    got_join_game = False
    got_sync_pos = False

    try:
        send_handshake(sock, host, port, next_state=2)
        login_start = encode_string(username) + (b"\x00" * 16)
        send_packet(sock, 0x00, login_start, compression_threshold=-1)

        state = "LOGIN"
        start = time.monotonic()
        while time.monotonic() - start < timeout_s:
            try:
                pid, payload = read_frame(sock, compression_threshold=compression_threshold)
            except socket.timeout:
                continue

            print(f"recv state={state} id=0x{pid:02X} len={len(payload)}")

            if state == "LOGIN":
                if pid == 0x00:
                    reason = read_string_payload(payload)
                    raise RuntimeError(f"login disconnect: {reason}")
                if pid == 0x03:
                    threshold, _ = decode_varint_from_bytes(payload, 0)
                    compression_threshold = threshold
                    print(f"set compression threshold={compression_threshold}")
                    continue
                if pid == 0x02:
                    send_packet(sock, 0x03, b"", compression_threshold=compression_threshold)  # Login Acknowledge
                    state = "CONFIG"
                    continue

            if state == "CONFIG":
                if pid == 0x02:
                    reason = read_string_payload(payload)
                    raise RuntimeError(f"config disconnect: {reason}")
                if pid == 0x04 and len(payload) == 8:
                    send_packet(sock, 0x04, payload, compression_threshold=compression_threshold)  # KeepAlive
                    continue
                if pid == 0x0E:
                    send_packet(sock, 0x07, payload, compression_threshold=compression_threshold)  # Known Packs (ack)
                    continue
                if pid == 0x03:
                    send_packet(sock, 0x03, b"", compression_threshold=compression_threshold)  # Finish ack
                    state = "PLAY"
                    continue
                if pid == 0x07:
                    continue  # registry data

            if state == "PLAY":
                if pid == 0x1D:
                    reason = read_string_payload(payload)
                    raise RuntimeError(f"play disconnect: {reason}")
                if pid == 0x2B:
                    got_join_game = True
                if pid == 0x26 and len(payload) == 8:
                    send_packet(sock, 0x18, payload, compression_threshold=compression_threshold)  # KeepAlive response
                if pid == 0x40:
                    if len(payload) < (8 * 3 + 4 * 2 + 1):
                        raise ValueError("sync pos payload too short")
                    teleport_id, _ = decode_varint_from_bytes(payload, 8 * 3 + 4 * 2 + 1)
                    send_packet(sock, 0x00, encode_varint(teleport_id), compression_threshold=compression_threshold)
                    got_sync_pos = True

                if got_join_game and got_sync_pos:
                    print("play ok (join game + sync pos)")
                    return 0

        raise TimeoutError("timeout waiting for PLAY (join game + sync pos)")
    finally:
        sock.close()


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Minimal protocol ping for mc_server (1.21.1).")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_status = sub.add_parser("status", help="Handshake STATUS + request + ping/pong")
    p_status.add_argument("host")
    p_status.add_argument("port", type=int)
    p_status.add_argument("--timeout", type=float, default=3.0)

    p_login = sub.add_parser("login", help="Handshake LOGIN + minimal CONFIG + verify PLAY start")
    p_login.add_argument("host")
    p_login.add_argument("port", type=int)
    p_login.add_argument("--username", default="Test")
    p_login.add_argument("--timeout", type=float, default=3.0)

    args = parser.parse_args(argv)

    if args.cmd == "status":
        return run_status(args.host, args.port, timeout_s=args.timeout)
    if args.cmd == "login":
        return run_login(args.host, args.port, username=args.username, timeout_s=args.timeout)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
