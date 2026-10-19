import sys
import tempfile
import unittest
import contextlib
import io
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from protocol import position, read_position, read_varint, varint
from stress import RateLimiter, block_target_offset, options, parse_perf, peak_one_second


class CodecTests(unittest.TestCase):
    def test_varint_roundtrip(self):
        for value in (0, 1, 127, 128, 775, 2**31 - 1, -1):
            self.assertEqual(read_varint(varint(value))[0], value)

    def test_position_roundtrip(self):
        for coords in ((0, 64, 0), (-3, -64, 5), (1234, 100, -789)):
            self.assertEqual(read_position(position(*coords)), coords)


class SafetyTests(unittest.TestCase):
    def test_peak_one_second(self):
        self.assertEqual(peak_one_second([0.0, 0.5, 0.9, 1.1, 2.5]), 3)

    def test_spawn_block_targets_are_distinct_and_in_reach(self):
        targets = [block_target_offset(index) for index in range(50)]
        self.assertEqual(len(targets), len(set(targets)))
        self.assertTrue(all(2 <= (x*x + z*z)**0.5 <= 4.5 for x, z in targets))

    def test_negative_block_cooldown_rejected(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            options(["--block-cooldown", "-1"])

    def test_block_mode_needs_consent(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            options(["--mode", "blocks"])

    def test_remote_block_mode_needs_separate_consent(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            options(["--mode", "blocks", "--allow-block-changes", "--host", "192.0.2.1"])

    def test_bot_cap(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            options(["--bots", "1001"])
        self.assertEqual(options(["--bots", "1000", "--mode", "connect"]).bots, 1000)

    def test_block_target_cap(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            options(["--bots", "51", "--mode", "blocks", "--allow-block-changes"])

    def test_block_rate_cap(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            options(["--mode", "blocks", "--allow-block-changes", "--block-rate", "401"])
        self.assertEqual(options(["--mode", "blocks", "--allow-block-changes", "--block-rate", "400"]).block_rate, 400)
        self.assertEqual(options(["--mode", "fastblocks", "--allow-block-changes"]).block_rate, 400)

    def test_perf_sample_only(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "server.log"
            path.write_text("perf tick: idx=1 total=60.0ms\nperf tick: idx=2 total=10.0ms\n"
                            "perf summary: ticks=2 avg_total=35.0ms max_total=60.0ms\n")
            result = parse_perf(path)
            self.assertEqual(result["sampled_ticks"], 2)
            self.assertEqual(result["sampled_ticks_over_50ms"], 1)
            self.assertEqual(result["last_summary_average_ms"], 35.0)


class RateLimiterTests(unittest.IsolatedAsyncioTestCase):
    async def test_shared_rate_limiter(self):
        import asyncio
        limiter = RateLimiter(20)

        class DummyConnection:
            async def send(self, packet_id, payload):
                self.last = (packet_id, payload)

        conn = DummyConnection()

        async def take():
            return await limiter.send(conn, 66, b"test")

        stamps = sorted(await asyncio.gather(*(take() for _ in range(4))))
        self.assertGreaterEqual(stamps[-1] - stamps[0], 0.13)

if __name__ == "__main__":
    unittest.main()
