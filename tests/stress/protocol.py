"""Small, bounded Minecraft Java 26.1.1 (protocol 775) wire codec.

Packet numbers are taken from src/generated/generated_minecraft_ids.h. This is
intentionally a GoatCore test client, not a general-purpose Minecraft library.
"""

import asyncio
import struct
import zlib

PROTOCOL = 775
MAX_FRAME = 16 * 1024 * 1024


def varint(value):
    value &= 0xFFFFFFFF
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)


def read_varint(data, offset=0):
    value = 0
    for i in range(5):
        if offset + i >= len(data):
            raise ValueError("truncated VarInt")
        byte = data[offset + i]
        value |= (byte & 0x7F) << (i * 7)
        if not byte & 0x80:
            return (value - 0x100000000 if value & 0x80000000 else value), offset + i + 1
    raise ValueError("VarInt too long")


def string(value):
    raw = value.encode("utf-8")
    return varint(len(raw)) + raw


def read_string(data, offset=0):
    length, offset = read_varint(data, offset)
    if length < 0 or offset + length > len(data):
        raise ValueError("invalid string length")
    return data[offset:offset + length].decode("utf-8", "replace"), offset + length


def position(x, y, z):
    packed = ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF)
    return struct.pack(">Q", packed)


def read_position(data):
    packed = struct.unpack_from(">Q", data)[0]
    x, y, z = packed >> 38, packed & 0xFFF, (packed >> 12) & 0x3FFFFFF
    return (x - (1 << 26) if x & (1 << 25) else x,
            y - (1 << 12) if y & (1 << 11) else y,
            z - (1 << 26) if z & (1 << 25) else z)


class Connection:
    def __init__(self, reader, writer):
        self.reader = reader
        self.writer = writer
        self.compression = -1
        self.sent = 0
        self.received = 0
        self.bytes_sent = 0
        self.bytes_received = 0

    async def send(self, packet_id, payload=b""):
        body = varint(packet_id) + payload
        if self.compression >= 0:
            if self.compression and len(body) >= self.compression:
                body = varint(len(body)) + zlib.compress(body)
            else:
                body = varint(0) + body
        frame = varint(len(body)) + body
        self.writer.write(frame)
        await self.writer.drain()
        self.sent += 1
        self.bytes_sent += len(frame)

    async def recv(self):
        length = 0
        for i in range(5):
            byte = (await self.reader.readexactly(1))[0]
            length |= (byte & 0x7F) << (7 * i)
            if not byte & 0x80:
                break
        else:
            raise ValueError("frame length VarInt too long")
        if not 0 < length <= MAX_FRAME:
            raise ValueError(f"invalid frame size: {length}")
        raw = await self.reader.readexactly(length)
        self.received += 1
        self.bytes_received += length + len(varint(length))
        if self.compression >= 0:
            size, offset = read_varint(raw)
            if size:
                if not 0 < size <= MAX_FRAME:
                    raise ValueError("invalid uncompressed size")
                decompressor = zlib.decompressobj()
                body = decompressor.decompress(raw[offset:], size + 1)
                if len(body) != size or not decompressor.eof or decompressor.unused_data:
                    raise ValueError("invalid compressed packet")
            else:
                body = raw[offset:]
        else:
            body = raw
        packet_id, offset = read_varint(body)
        return packet_id, body[offset:]

    async def close(self):
        self.writer.close()
        try:
            await asyncio.wait_for(self.writer.wait_closed(), 2)
        except (TimeoutError, OSError):
            pass
