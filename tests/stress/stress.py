#!/usr/bin/env python3
"""Concurrent, external GoatCore 26.1.1 protocol-775 load client."""

import argparse
import asyncio
from collections import defaultdict
from datetime import datetime, timezone
import ipaddress
import json
import math
from pathlib import Path
import re
import secrets
import statistics
import struct
import time

from protocol import Connection, PROTOCOL, position, read_position, read_string, read_varint, string, varint

HERE = Path(__file__).resolve().parent
QA_POS = re.compile(rb"QA state pos=(-?\d+\.\d+) (-?\d+\.\d+) (-?\d+\.\d+)")
QA_BLOCK = re.compile(rb"QA block x=(-?\d+) y=(-?\d+) z=(-?\d+) state=(-?\d+)")


def local_host(host):
    if host.lower() == "localhost":
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def block_target_offset(index):
    """Give each bot a distinct target within reach of the shared spawn."""
    offsets = [(x, z) for x in range(-4, 5) for z in range(-4, 5)
               if 2 <= math.hypot(x, z) <= 4.5]
    offsets.sort(key=lambda point: (math.hypot(*point), point[0], point[1]))
    return offsets[index]


def peak_one_second(events):
    times = sorted(events)
    left = 0
    peak = 0
    for right, stamp in enumerate(times):
        while stamp - times[left] >= 1.0:
            left += 1
        peak = max(peak, right - left + 1)
    return peak


class RateLimiter:
    def __init__(self, rate):
        self.interval = 1.0 / rate
        self.next_at = 0.0
        self.lock = asyncio.Lock()

    async def send(self, conn, packet_id, payload):
        async with self.lock:
            delay = self.next_at - time.monotonic()
            if delay > 0:
                await asyncio.sleep(delay)
            await conn.send(packet_id, payload)
            sent_at = time.monotonic()
            self.next_at = sent_at + self.interval
            return sent_at


def parse_perf(path):
    if not path:
        return None
    rows = []
    summaries = []
    connections = []
    flushes = []
    update_sends = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "perf tick:" in line:
            match = re.search(r"\btotal=([0-9.]+)ms", line)
            if match:
                rows.append(float(match.group(1)))
                for key, dest in (("conns", connections), ("net_flushes", flushes),
                                  ("update_sends", update_sends)):
                    value = re.search(rf"\b{key}=(\d+)", line)
                    if value:
                        dest.append(int(value.group(1)))
        elif "perf summary:" in line:
            match = re.search(r"\bavg_total=([0-9.]+)ms.*\bmax_total=([0-9.]+)ms", line)
            if match:
                summaries.append((float(match.group(1)), float(match.group(2))))
    return {
        "sampled_ticks": len(rows),
        "sampled_tick_average_ms": statistics.mean(rows) if rows else None,
        "sampled_tick_max_ms": max(rows) if rows else None,
        "sampled_ticks_over_50ms": sum(v > 50 for v in rows),
        "summary_rows": len(summaries),
        "last_summary_average_ms": summaries[-1][0] if summaries else None,
        "last_summary_max_ms": summaries[-1][1] if summaries else None,
        "sampled_max_connections": max(connections) if connections else None,
        "sampled_network_flushes": sum(flushes) if flushes else None,
        "sampled_update_sends": sum(update_sends) if update_sends else None,
        "note": "Tick samples are logged selectively; sampled counts are not all server ticks.",
    }


class Bot:
    def __init__(self, index, args, log):
        self.index = index
        # Fresh identity per run: saved player positions from old tests must not
        # move the next test away from the world's actual spawn.
        self.name = f"Stress{args.run_id}{index:05d}"
        self.args = args
        self.log = log
        self.conn = None
        self.state = "new"
        self.spawn = asyncio.Event()
        self.origin_chunk = asyncio.Event()
        self.chunks = asyncio.Event()
        self.reader_task = None
        self.error = None
        self.pos = None
        self.origin = None
        self.joined = False
        self.gamemode = None
        self.started = None
        self.play_at = None
        self.spawn_at = None
        self.origin_chunk_at = None
        self.chunk_packets = 0
        self.position_corrections = 0
        self.move_send_times = []
        self.closed = None
        self.expected_close = False
        self.actions = 0
        self.cycles_completed = 0
        self.movements = 0
        self.movement_confirmed = 0
        self.placed = 0
        self.broken = 0
        self.place_verified_times = []
        self.break_verified_times = []
        self.place_send_times = []
        self.break_send_times = []
        self.fast_target = None
        self.fast_state = None
        self.block_prepared = False
        self.shared_placed_observed = 0
        self.shared_broken_observed = 0
        self.failures = 0
        self.latencies = []
        self.block_events = defaultdict(asyncio.Queue)
        self.slot_events = asyncio.Queue()
        self.qa_positions = asyncio.Queue()
        self.qa_messages = asyncio.Queue()
        self.sequence = 0
        self.stop_at = None

    async def connect(self):
        self.started = time.monotonic()
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(self.args.host, self.args.port), self.args.timeout)
        self.conn = Connection(reader, writer)
        await self.conn.send(0, varint(PROTOCOL) + string(self.args.host) +
                             struct.pack(">H", self.args.port) + varint(2))
        await self.conn.send(0, string(self.name) + b"\0" * 16)
        self.state = "login"
        self.reader_task = asyncio.create_task(self.read_loop())
        await asyncio.wait_for(self.spawn.wait(), self.args.timeout)
        if self.error:
            raise RuntimeError(self.error)
        await asyncio.wait_for(self.origin_chunk.wait(), self.args.timeout)
        if self.error:
            raise RuntimeError(self.error)
        self.state = "play"
        self.origin = self.pos
        self.play_at = time.monotonic()
        if self.args.shared_zone and self.index == 0:
            self.args.shared_target = (math.floor(self.origin[0]) + 2,
                                       math.floor(self.origin[1]), math.floor(self.origin[2]))
            self.args.shared_ready.set()
        self.log(f"{self.name} spawn {self.pos} game_mode={self.gamemode}")

    async def read_loop(self):
        try:
            while True:
                packet_id, payload = await self.conn.recv()
                if self.state == "login":
                    if packet_id == 0:
                        reason, _ = read_string(payload)
                        raise RuntimeError(f"login disconnect: {reason}")
                    if packet_id == 3:
                        self.conn.compression, _ = read_varint(payload)
                    elif packet_id == 2:
                        await self.conn.send(3)
                        self.state = "config"
                elif self.state == "config":
                    if packet_id == 2:
                        reason, _ = read_string(payload)
                        raise RuntimeError(f"config disconnect: {reason}")
                    if packet_id == 4 and len(payload) == 8:
                        await self.conn.send(4, payload)
                    elif packet_id == 5 and len(payload) == 4:
                        await self.conn.send(5, payload)
                    elif packet_id == 14:
                        await self.conn.send(7, payload)
                    elif packet_id == 3:
                        await self.conn.send(3)
                        self.state = "joining"
                else:
                    await self.play_packet(packet_id, payload)
        except (asyncio.IncompleteReadError, ConnectionError, OSError, ValueError, RuntimeError) as exc:
            if not self.expected_close:
                self.error = str(exc) or type(exc).__name__
                self.log(f"{self.name} error: {self.error}")
        finally:
            self.closed = time.monotonic()
            self.spawn.set()
            self.state = "closed"

    async def play_packet(self, packet_id, payload):
        if packet_id == 32:
            try:
                reason, _ = read_string(payload)
            except ValueError:
                reason = payload[:200].hex()
            raise RuntimeError(f"play disconnect: {reason}")
        if packet_id == 49:  # login / join game
            self.joined = True
            offset = 5
            count, offset = read_varint(payload, offset)
            for _ in range(count):
                _, offset = read_string(payload, offset)
            _, offset = read_varint(payload, offset)  # max players
            _, offset = read_varint(payload, offset)  # view distance
            _, offset = read_varint(payload, offset)  # simulation distance
            offset += 3 + 1  # three booleans, dimension type
            _, offset = read_string(payload, offset)
            self.gamemode = payload[offset + 8]
        elif packet_id == 72:  # teleport id + absolute position
            teleport, offset = read_varint(payload)
            if self.spawn_at is not None:
                self.position_corrections += 1
            self.pos = struct.unpack_from(">ddd", payload, offset)
            await self.conn.send(0, varint(teleport))
            if self.joined:
                if self.spawn_at is None:
                    self.spawn_at = time.monotonic()
                self.spawn.set()
        elif packet_id == 45 and self.pos is not None and len(payload) >= 8:
            self.chunk_packets += 1
            chunk_x, chunk_z = struct.unpack_from(">ii", payload)
            if (chunk_x, chunk_z) == (math.floor(self.pos[0]) // 16,
                                       math.floor(self.pos[2]) // 16):
                if self.origin_chunk_at is None:
                    self.origin_chunk_at = time.monotonic()
                self.origin_chunk.set()
        elif packet_id == 44 and len(payload) == 8:
            await self.conn.send(28, payload)
        elif packet_id == 61 and len(payload) == 4:
            await self.conn.send(45, payload)  # Pong, protocol 775
        elif packet_id == 11:  # chunk batch finished
            self.chunks.set()
            await self.conn.send(11, struct.pack(">f", 4.0))
        elif packet_id == 8 and len(payload) >= 9:
            pos = read_position(payload)
            state, _ = read_varint(payload, 8)
            if self.args.mode == "fastblocks":
                if pos == self.fast_target:
                    self.block_events[pos].put_nowait(state)
                    if self.block_prepared and state != self.fast_state:
                        if state:
                            self.placed += 1
                            self.place_verified_times.append(time.monotonic())
                        else:
                            self.broken += 1
                            self.break_verified_times.append(time.monotonic())
                    self.fast_state = state
            else:
                self.log(f"{self.name} block update pos={pos} state={state}")
                self.block_events[pos].put_nowait(state)
        elif packet_id == 20 and len(payload) >= 4:
            # ContainerSetSlot: window byte, state VarInt, slot short, Slot.
            offset = 1
            _, offset = read_varint(payload, offset)
            slot = struct.unpack_from(">h", payload, offset)[0]
            offset += 2
            count, offset = read_varint(payload, offset)
            item = read_varint(payload, offset)[0] if count else 0
            self.slot_events.put_nowait((slot, item, count))
        elif packet_id == 121:
            if b"QA block" in payload:
                self.qa_messages.put_nowait(payload)
            match = QA_POS.search(payload)
            if match:
                self.qa_positions.put_nowait(tuple(float(x) for x in match.groups()))
            elif self.args.qa_verify_movement:
                self.log(f"{self.name} system message: {payload[:160]!r}")

    async def move(self, x, z):
        if self.pos is None:
            raise RuntimeError("no spawn position")
        old_x, y, old_z = self.pos
        dx, dz = x - old_x, z - old_z
        distance = math.hypot(dx, dz)
        steps = max(1, math.ceil(distance / 0.25))
        for step in range(1, steps + 1):
            px = old_x + dx * step / steps
            pz = old_z + dz * step / steps
            await self.conn.send(30, struct.pack(">ddd?", px, y, pz, True))
            self.move_send_times.append(time.monotonic())
            self.movements += 1
            self.pos = (px, y, pz)
            await asyncio.sleep(0.05)  # <= 5 blocks/s
        if self.args.qa_verify_movement:
            while not self.qa_positions.empty():
                self.qa_positions.get_nowait()
            await self.conn.send(9, string("!qa state"))
            try:
                observed = await asyncio.wait_for(self.qa_positions.get(), self.args.timeout)
                if math.dist(observed, self.pos) <= 0.2:
                    self.movement_confirmed += 1
                else:
                    self.failures += 1
            except TimeoutError:
                self.failures += 1

    async def setup_block_item(self):
        if self.gamemode != 1:
            raise RuntimeError("block mode needs creative gamemode (use a temporary creative world)")
        # Player inventory hotbar slot 36, stone item registry id 1.
        await self.conn.send(56, struct.pack(">h", 36) + varint(64) + varint(1) + varint(0) + varint(0))
        while True:
            slot, item, count = await asyncio.wait_for(self.slot_events.get(), self.args.timeout)
            if slot == 36:
                if item != 1 or count < 1:
                    raise RuntimeError(f"stone inventory rejected: item={item} count={count}")
                break
        await self.conn.send(53, struct.pack(">h", 0))

    def clear_block_events(self, block):
        queue = self.block_events[block]
        while not queue.empty():
            queue.get_nowait()

    async def expect_block(self, block, predicate):
        queue = self.block_events[block]
        started = time.monotonic()
        try:
            while True:
                remaining = self.args.timeout - (time.monotonic() - started)
                if self.stop_at is not None:
                    remaining = min(remaining, self.stop_at - time.monotonic())
                if remaining <= 0:
                    return None
                state = await asyncio.wait_for(queue.get(), remaining)
                if predicate(state):
                    self.latencies.append((time.monotonic() - started) * 1000)
                    return state
        except TimeoutError:
            return None

    async def block_cycle(self, block):
        bx, by, bz = block
        base = (bx, by - 1, bz)
        # Prepare each private target once; repeated preparation would add
        # unmetered break packets to the high-rate scenario.
        if not self.block_prepared or self.args.shared_zone:
            self.clear_block_events(block)
            self.sequence += 1
            await self.conn.send(41, varint(0) + position(*block) + varint(1) + varint(self.sequence))
            if await self.expect_block(block, lambda state: state == 0) is None:
                self.failures += 1
                return False
            self.block_prepared = True
        self.clear_block_events(block)
        self.sequence += 1
        place = varint(0) + position(*base) + varint(1) + struct.pack(">fff?", 0.5, 1.0, 0.5, False) + varint(self.sequence)
        if self.args.place_limiter:
            sent_at = await self.args.place_limiter.send(self.conn, 66, place)
        else:
            await self.conn.send(66, place)
            sent_at = time.monotonic()
        self.place_send_times.append(sent_at)
        if await self.expect_block(block, lambda state: state != 0) is None:
            self.failures += 1
            return False
        if self.args.shared_zone:
            self.shared_placed_observed += 1
        else:
            self.placed += 1
            self.place_verified_times.append(time.monotonic())
        await asyncio.sleep(self.args.block_cooldown)
        self.clear_block_events(block)
        self.sequence += 1
        break_payload = varint(0) + position(*block) + varint(1) + varint(self.sequence)
        if self.args.break_limiter:
            sent_at = await self.args.break_limiter.send(self.conn, 41, break_payload)
        else:
            await self.conn.send(41, break_payload)
            sent_at = time.monotonic()
        self.break_send_times.append(sent_at)
        if await self.expect_block(block, lambda state: state == 0) is None:
            self.failures += 1
            return False
        if self.args.shared_zone:
            self.shared_broken_observed += 1
        else:
            self.broken += 1
            self.break_verified_times.append(time.monotonic())
        await asyncio.sleep(self.args.block_cooldown)
        return True

    async def fast_block_cycle(self, block):
        base = (block[0], block[1] - 1, block[2])
        if not self.block_prepared:
            self.fast_target = block
            self.clear_block_events(block)
            self.sequence += 1
            await self.conn.send(41, varint(0) + position(*block) + varint(1) + varint(self.sequence))
            if await self.expect_block(block, lambda state: state == 0) is None:
                self.failures += 1
                return
            self.block_prepared = True
            self.fast_state = 0
        self.sequence += 1
        place = varint(0) + position(*base) + varint(1) + struct.pack(">fff?", 0.5, 1.0, 0.5, False) + varint(self.sequence)
        sent_at = await self.args.place_limiter.send(self.conn, 66, place)
        self.place_send_times.append(sent_at)
        await asyncio.sleep(0.055)
        self.sequence += 1
        sent_at = await self.args.break_limiter.send(
            self.conn, 41, varint(0) + position(*block) + varint(1) + varint(self.sequence))
        self.break_send_times.append(sent_at)

    async def work(self, stop):
        self.stop_at = stop if self.args.cycles_per_bot is not None else None
        try:
            await self.connect()
            if self.args.shared_zone:
                await asyncio.wait_for(self.args.shared_ready.wait(), self.args.timeout)
            if self.args.mode in ("blocks", "mixed", "fastblocks"):
                await asyncio.wait_for(self.chunks.wait(), self.args.timeout)
                await self.setup_block_item()
            center_x, center_z = self.pos[0], self.pos[2]
            phase = 0
            while (time.monotonic() < stop and self.state == "play" and
                   (self.args.cycles_per_bot is None or self.cycles_completed < self.args.cycles_per_bot)):
                self.actions += 1
                if self.args.mode == "movement":
                    angle = phase * math.pi / 2 + 2 * math.pi * self.index / self.args.bots
                    x = center_x + self.args.radius * math.cos(angle)
                    z = center_z + self.args.radius * math.sin(angle)
                    await self.move(x, z)
                if self.args.mode in ("blocks", "mixed", "fastblocks") and time.monotonic() < stop:
                    if self.args.shared_zone:
                        bx, _, bz = self.args.shared_target
                    else:
                        ox, oz = block_target_offset(self.index)
                        bx = math.floor(center_x) + ox
                        bz = math.floor(center_z) + oz
                    block = (bx, math.floor(self.pos[1]), bz)
                    if self.args.mode == "fastblocks":
                        await self.fast_block_cycle(block)
                    else:
                        if await self.block_cycle(block):
                            self.cycles_completed += 1
                phase += 1
                if self.args.mode != "fastblocks":
                    await asyncio.sleep(min(self.args.interval, max(0, stop - time.monotonic())))
            if self.args.mode == "fastblocks":
                await asyncio.sleep(0.5)
        except (TimeoutError, ConnectionError, OSError, ValueError, RuntimeError) as exc:
            self.error = str(exc) or type(exc).__name__
            self.log(f"{self.name} error: {self.error}")
        finally:
            if self.conn:
                self.expected_close = True
                await self.conn.close()
            if self.reader_task:
                try:
                    await asyncio.wait_for(self.reader_task, 2)
                except TimeoutError:
                    self.reader_task.cancel()
            self.closed = self.closed or time.monotonic()

    def report(self):
        intervals = [(b - a) * 1000 for a, b in zip(self.move_send_times, self.move_send_times[1:])]
        return {
            "name": self.name, "connected": self.play_at is not None,
            "error": self.error, "connection_seconds":
            round(self.play_at - self.started, 3) if self.started and self.play_at else None,
            "session_seconds": round((self.closed or time.monotonic()) - self.started, 3) if self.started else None,
            "spawn_seconds": round(self.spawn_at - self.started, 3) if self.started and self.spawn_at else None,
            "origin_chunk_seconds": round(self.origin_chunk_at - self.started, 3)
            if self.started and self.origin_chunk_at else None,
            "chunks_received": self.chunk_packets,
            "position_corrections": self.position_corrections,
            "movement_interval_ms_mean": statistics.mean(intervals) if intervals else None,
            "movement_interval_ms_max": max(intervals) if intervals else None,
            "movements_sent": self.movements,
            "movement_confirmed": self.movement_confirmed if self.args.qa_verify_movement else None,
            "blocks_placed_verified": self.placed, "blocks_broken_verified": self.broken,
            "cycles_completed": self.cycles_completed,
            "block_place_packets_sent": len(self.place_send_times),
            "block_break_packets_sent": len(self.break_send_times),
            "shared_place_observations": self.shared_placed_observed,
            "shared_break_observations": self.shared_broken_observed,
            "actions_failed": self.failures, "packets_sent": self.conn.sent if self.conn else 0,
            "packets_received": self.conn.received if self.conn else 0,
            "bytes_sent": self.conn.bytes_sent if self.conn else 0,
            "bytes_received": self.conn.bytes_received if self.conn else 0,
            "operation_latency_ms_mean": statistics.mean(self.latencies) if self.latencies else None,
        }


def options(argv=None):
    parser = argparse.ArgumentParser(description="GoatCore 26.1.1 external multiplayer stress test")
    parser.add_argument("--bots", type=int, default=1)
    parser.add_argument("--max-bots", type=int, default=1000)
    parser.add_argument("--duration", type=float, default=30)
    parser.add_argument("--cycles-per-bot", type=int,
                        help="fixed verified place+break cycles per bot; --duration is a hard deadline (blocks mode)")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=25565)
    parser.add_argument("--radius", type=float, default=3)
    parser.add_argument("--interval", type=float, default=1)
    parser.add_argument("--block-cooldown", type=float, default=1,
                        help="seconds to leave each placed/cleared block visible")
    parser.add_argument("--block-rate", type=float,
                        help="aggregate cap per second, separately for verified poses and casses (max 400)")
    parser.add_argument("--mode", choices=("connect", "movement", "blocks", "mixed", "fastblocks"), default="movement")
    parser.add_argument("--spawn-delay", type=float, default=0.1)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--server-log", type=Path, help="optional MC_PERF server log")
    parser.add_argument("--allow-block-changes", action="store_true")
    parser.add_argument("--allow-remote-destructive", action="store_true")
    parser.add_argument("--shared-zone", action="store_true", help="intentional concurrent block target")
    parser.add_argument("--qa-verify-movement", action="store_true", help="requires MC_QA_MODE=1; asks !qa state")
    parser.add_argument("--timeout", type=float, default=15)
    args = parser.parse_args(argv)
    if not 1 <= args.bots <= args.max_bots <= 1000:
        parser.error("bots must be in 1..max-bots, with max-bots <= 1000")
    if (not all(math.isfinite(v) for v in (args.duration, args.radius, args.interval, args.block_cooldown,
                                            args.spawn_delay, args.timeout)) or
            args.duration <= 0 or args.radius < 0 or args.interval <= 0 or args.block_cooldown < 0 or
            args.spawn_delay < 0 or args.timeout <= 0):
        parser.error("invalid duration/radius/interval/block-cooldown/spawn-delay/timeout")
    if args.block_rate is not None and (not math.isfinite(args.block_rate) or not 0 < args.block_rate <= 400):
        parser.error("block-rate must be in (0, 400]")
    if args.mode == "fastblocks" and args.block_rate is None:
        args.block_rate = 400.0
    if args.block_rate is not None and args.mode not in ("blocks", "mixed", "fastblocks"):
        parser.error("block-rate requires a block mode")
    if not 1 <= args.port <= 65535:
        parser.error("port must be 1..65535")
    if args.mode in ("blocks", "mixed", "fastblocks") and not args.allow_block_changes:
        parser.error("block modes require --allow-block-changes")
    if args.mode in ("blocks", "mixed", "fastblocks") and args.bots > 50:
        parser.error("block modes are limited to 50 distinct targets near spawn")
    if args.mode in ("blocks", "mixed", "fastblocks") and not local_host(args.host) and not args.allow_remote_destructive:
        parser.error("remote block modes require --allow-remote-destructive")
    if args.shared_zone and args.mode not in ("blocks", "mixed"):
        parser.error("--shared-zone is only valid for block modes")
    if args.cycles_per_bot is not None and (args.cycles_per_bot <= 0 or args.mode != "blocks" or args.shared_zone):
        parser.error("--cycles-per-bot requires private-target blocks mode and a positive count")
    return args


async def run(args):
    args.run_id = secrets.token_hex(2)
    args.place_limiter = RateLimiter(args.block_rate) if args.block_rate else None
    args.break_limiter = RateLimiter(args.block_rate) if args.block_rate else None
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + args.run_id
    result_dir = HERE / "results" / stamp
    result_dir.mkdir(parents=True, exist_ok=False)
    log_path = result_dir / "bots.log"
    with log_path.open("w", encoding="utf-8") as log_file:
        def log(message):
            log_file.write(datetime.now(timezone.utc).isoformat() + " " + message + "\n")
            log_file.flush()

        bots = [Bot(i, args, log) for i in range(args.bots)]
        args.shared_ready = asyncio.Event()
        args.shared_target = None
        started = time.monotonic()
        stop = started + args.duration
        tasks = []
        max_connected = 0
        for bot in bots:
            if time.monotonic() >= stop:
                break
            tasks.append(asyncio.create_task(bot.work(stop)))
            await asyncio.sleep(min(args.spawn_delay, max(0, stop - time.monotonic())))
        while any(not task.done() for task in tasks):
            elapsed = time.monotonic() - started
            connected = sum(b.state == "play" for b in bots)
            max_connected = max(max_connected, connected)
            failed = sum(b.error is not None and b.pos is None for b in bots)
            errors = sum(b.error is not None for b in bots) + sum(b.failures for b in bots)
            print(f"\rBOTS CONNECTÉS {connected:3d} | EN ACTION {sum(b.state == 'play' and b.actions > 0 for b in bots):3d}"
                  f" | CONNEXIONS ÉCHOUÉES {failed:3d} | BLOCS POSÉS {sum(b.placed for b in bots):4d}"
                  f" | BLOCS CASSÉS {sum(b.broken for b in bots):4d} | ERREURS {errors:3d}"
                  f" | DURÉE {elapsed:6.1f}s", end="", flush=True)
            await asyncio.sleep(0.5)
        await asyncio.gather(*tasks)
        print()
        rows = [bot.report() for bot in bots]
        latencies = [value for bot in bots for value in bot.latencies]
        durations = [row["connection_seconds"] for row in rows if row["connected"]]
        spawn_durations = [row["spawn_seconds"] for row in rows if row["spawn_seconds"] is not None]
        chunk_durations = [row["origin_chunk_seconds"] for row in rows if row["origin_chunk_seconds"] is not None]
        perf = parse_perf(args.server_log)
        placed_peak = peak_one_second(t for bot in bots for t in bot.place_verified_times)
        broken_peak = peak_one_second(t for bot in bots for t in bot.break_verified_times)
        place_send_peak = peak_one_second(t for bot in bots for t in bot.place_send_times)
        break_send_peak = peak_one_second(t for bot in bots for t in bot.break_send_times)
        report = {
            "protocol": PROTOCOL, "version": "26.1.1", "host": args.host, "port": args.port,
            "mode": args.mode, "requested_bots": args.bots, "started_bots": len(tasks),
            "bots_not_started": args.bots - len(tasks),
            "duration_seconds": round(time.monotonic() - started, 3),
            "connected_successfully": sum(row["connected"] for row in rows),
            "connections_failed": sum(row["error"] is not None and not row["connected"] for row in rows),
            "unexpected_disconnects": sum(row["error"] is not None and row["connected"] for row in rows),
            "maximum_concurrent_connected": None,
            "movements_sent": sum(row["movements_sent"] for row in rows),
            "movements_confirmed": sum(row["movement_confirmed"] or 0 for row in rows) if args.qa_verify_movement else None,
            "blocks_placed_verified": sum(row["blocks_placed_verified"] for row in rows),
            "blocks_broken_verified": sum(row["blocks_broken_verified"] for row in rows),
            "requested_cycles": args.bots * args.cycles_per_bot if args.cycles_per_bot is not None else None,
            "completed_cycles": sum(row["cycles_completed"] for row in rows),
            "missing_cycles": sum(max(0, args.cycles_per_bot - row["cycles_completed"]) for row in rows)
            if args.cycles_per_bot is not None else None,
            "block_place_packets_sent": sum(row["block_place_packets_sent"] for row in rows),
            "block_break_packets_sent": sum(row["block_break_packets_sent"] for row in rows),
            "place_packets_peak_one_second": place_send_peak,
            "break_packets_peak_one_second": break_send_peak,
            "block_rate_cap_per_action": args.block_rate,
            "placed_peak_one_second": placed_peak,
            "broken_peak_one_second": broken_peak,
            "shared_place_observations": sum(row["shared_place_observations"] for row in rows),
            "shared_break_observations": sum(row["shared_break_observations"] for row in rows),
            "actions_failed": sum(row["actions_failed"] for row in rows),
            "packets_sent": sum(row["packets_sent"] for row in rows),
            "packets_received": sum(row["packets_received"] for row in rows),
            "packets_sent_per_second": sum(row["packets_sent"] for row in rows) / max(0.001, time.monotonic() - started),
            "packets_received_per_second": sum(row["packets_received"] for row in rows) / max(0.001, time.monotonic() - started),
            "connection_seconds_mean": statistics.mean(durations) if durations else None,
            "spawn_seconds_mean": statistics.mean(spawn_durations) if spawn_durations else None,
            "origin_chunk_seconds_mean": statistics.mean(chunk_durations) if chunk_durations else None,
            "chunks_received": sum(row["chunks_received"] for row in rows),
            "position_corrections": sum(row["position_corrections"] for row in rows),
            "operation_latency_ms_mean": statistics.mean(latencies) if latencies else None,
            "operation_latency_ms_max": max(latencies) if latencies else None,
            "server_perf": perf, "bots": rows,
            "limitations": ["Movement confirmation requires --qa-verify-movement and MC_QA_MODE=1.",
                            "MC_PERF sampled ticks are not a complete tick histogram."],
        }
        events = sorted((event for bot in bots if bot.play_at is not None
                         for event in ((bot.play_at, 1), (bot.closed or time.monotonic(), -1))))
        concurrent = 0
        for _, delta in events:
            concurrent += delta
            max_connected = max(max_connected, concurrent)
        report["maximum_concurrent_connected"] = max_connected
        summary = (f"GoatCore stress {stamp}\nmode={args.mode} bots={args.bots} duration={report['duration_seconds']}s\n"
                   f"connected={report['connected_successfully']} connection_failures={report['connections_failed']} "
                   f"unexpected_disconnects={report['unexpected_disconnects']}\n"
                   f"movements_sent={report['movements_sent']} movements_confirmed={report['movements_confirmed']}\n"
                   f"blocks_placed_verified={report['blocks_placed_verified']} "
                   f"blocks_broken_verified={report['blocks_broken_verified']} action_failures={report['actions_failed']} "
                   f"cycles={report['completed_cycles']}/{report['requested_cycles']} missing={report['missing_cycles']}\n"
                   f"peak_1s_sent_place={place_send_peak} peak_1s_sent_break={break_send_peak} "
                   f"peak_1s_confirmed_place={placed_peak} peak_1s_confirmed_break={broken_peak} "
                   f"cap_per_action={args.block_rate}\n")
        (result_dir / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        (result_dir / "summary.txt").write_text(summary, encoding="utf-8")
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(summary, end="")
        print(f"Reports: {result_dir}")
        return 1 if (report["connected_successfully"] != args.bots or
                     report["bots_not_started"] or report["connections_failed"] or
                     report["unexpected_disconnects"] or report["actions_failed"] or report["missing_cycles"]) else 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(run(options())))
