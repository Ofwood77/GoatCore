#!/usr/bin/env python3
"""Integration check for epoll ownership, round-robin, stale events and shutdown."""

import os
import re
import signal
import socket
import subprocess
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from test_network_epoll import exercise


ROOT = Path(__file__).resolve().parents[1]
WORKER_LINE = re.compile(
    r"perf network: worker=(\d+) connections=(\d+) peak=(\d+) "
    r"accepted=(\d+) disconnected=(\d+)"
)


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def run_case(workers, one_cpu=False):
    port = free_port()
    name = "four_one_cpu" if one_cpu else {1: "one", 2: "two", 4: "four"}[workers]
    with tempfile.TemporaryDirectory(prefix=f"goatcore-epoll-{workers}-") as world:
        env = os.environ.copy()
        env.update({
            "MC_SETUP_FILE": str(ROOT / f"tests/stress/network/{name}.conf"),
            "MC_WORLD_PATH": world,
            "MC_BIND_PORT": str(port),
            "MC_PERF": "1",
            "MC_PERF_SLOW_MS": "1000",
            "MC_PERF_SUMMARY_TICKS": "10",
        })
        proc = subprocess.Popen(
            [str(ROOT / "mc_server")], cwd=ROOT, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        output = ""
        try:
            deadline = time.monotonic() + 8
            while True:
                if proc.poll() is not None:
                    raise AssertionError(f"server exited before listening: {proc.returncode}")
                try:
                    exercise("127.0.0.1", port, 2)
                    break
                except (ConnectionRefusedError, OSError):
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(0.05)

            if one_cpu:
                allowed = os.sched_getaffinity(proc.pid)
                assert len(allowed) == 1, f"expected one allowed CPU, got {allowed}"
                for tid in (Path("/proc") / str(proc.pid) / "task").iterdir():
                    assert os.sched_getaffinity(int(tid.name)) == allowed

            with ThreadPoolExecutor(max_workers=24) as pool:
                list(pool.map(lambda i: exercise("127.0.0.1", port, i), range(80)))
            # Allow a full MC_PERF window after the final disconnect.
            time.sleep(0.7)
            proc.send_signal(signal.SIGTERM)
            output, _ = proc.communicate(timeout=10)
            assert proc.returncode == 0, f"shutdown failed: {proc.returncode}"
            rows = {}
            for worker, connections, peak, accepted, disconnected in WORKER_LINE.findall(output):
                rows[int(worker)] = tuple(map(int, (connections, peak, accepted, disconnected)))
            assert len(rows) == workers, f"missing worker metrics: {rows}"
            counts = [rows[i][2] for i in range(workers)]
            assert sum(counts) == 81, f"lost/duplicated accepts: {counts}"
            assert max(counts) - min(counts) <= 1, f"round-robin imbalance: {counts}"
            assert all(rows[i][0] == 0 and rows[i][3] == rows[i][2] for i in range(workers)), rows
            print(f"multi-epoll: {workers} workers, accepted={counts}, "
                  f"one_cpu={one_cpu}, clean shutdown")
        except Exception:
            if proc.poll() is None:
                proc.kill()
            more, _ = proc.communicate(timeout=10)
            output += more
            raise AssertionError(f"multi-epoll {workers} workers failed; server log:\n{output[-5000:]}")


def shutdown_under_load():
    port = free_port()
    with tempfile.TemporaryDirectory(prefix="goatcore-epoll-stop-") as world:
        env = os.environ.copy()
        env.update({
            "MC_SETUP_FILE": str(ROOT / "tests/stress/network/four.conf"),
            "MC_WORLD_PATH": world,
            "MC_BIND_PORT": str(port),
            "MC_MAX_CONNECTIONS": "128",
        })
        proc = subprocess.Popen(
            [str(ROOT / "mc_server")], cwd=ROOT, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        sockets = []
        try:
            deadline = time.monotonic() + 8
            while True:
                try:
                    probe = socket.create_connection(("127.0.0.1", port), timeout=0.5)
                    probe.close()
                    break
                except ConnectionRefusedError:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(0.05)
            for _ in range(100):
                sock = socket.create_connection(("127.0.0.1", port), timeout=2)
                sock.settimeout(5)
                sock.sendall(b"\x80")  # incomplete frame; often still in an epoll batch
                sockets.append(sock)
            proc.send_signal(signal.SIGTERM)
            output, _ = proc.communicate(timeout=10)
            assert proc.returncode == 0, f"server did not stop cleanly: {output[-3000:]}"
            for sock in sockets:
                try:
                    assert sock.recv(1) == b"", "socket survived shutdown"
                except ConnectionResetError:
                    # Linux may send RST when closing a socket with unread data.
                    pass
            print("multi-epoll: 4 workers, 100 partial frames, clean shutdown")
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.communicate(timeout=10)
            for sock in sockets:
                sock.close()


if __name__ == "__main__":
    for count in (1, 2, 4):
        run_case(count)
    run_case(4, one_cpu=True)
    shutdown_under_load()
