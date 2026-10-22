#!/usr/bin/env python3
"""Sequential, same-load 1/2/4-worker benchmark on disposable flat worlds."""

import json
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RESULTS = ROOT / "tests/stress/results"
SUMMARY = re.compile(
    r"perf summary: ticks=(\d+) tps=([\d.]+).*?p95_total=([\d.]+)ms "
    r"p99_total=([\d.]+)ms.*?over_50ms=(\d+).*?proto_ticks=(\d+)"
)
WORKER = re.compile(
    r"perf network: worker=(\d+) connections=(\d+) peak=(\d+) "
    r"accepted=(\d+) disconnected=(\d+) packets=(\d+) bytes_in=(\d+) "
    r"bytes_out=(\d+).*?task_wait_avg=([\d.]+)ms.*?cpu=([\d.]+)%"
)


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def benchmark(count, result_dir):
    name = {1: "one", 2: "two", 4: "four"}[count]
    port = free_port()
    world = result_dir / f"world-{name}"
    world.mkdir()
    report_path = result_dir / f"report-{name}.json"
    log_path = result_dir / f"server-{name}.log"
    env = os.environ.copy()
    env.update({
        "MC_SETUP_FILE": str(ROOT / f"tests/stress/network/{name}.conf"),
        "MC_WORLD_PATH": str(world),
        "MC_BIND_PORT": str(port),
        "MC_PERF": "1",
        "MC_PERF_SLOW_MS": "1000",
        "MC_PERF_SUMMARY_TICKS": "100",
    })
    with log_path.open("w", encoding="utf-8") as log_file:
        proc = subprocess.Popen([str(ROOT / "mc_server")], cwd=ROOT, env=env,
                                stdout=log_file, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 10
            while True:
                if proc.poll() is not None:
                    raise RuntimeError(f"server exited before listening: {proc.returncode}")
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                        break
                except ConnectionRefusedError:
                    if time.monotonic() > deadline:
                        raise
                    time.sleep(0.05)
            command = [sys.executable, str(ROOT / "tests/stress/stress.py"),
                       "--host", "127.0.0.1", "--port", str(port),
                       "--bots", "50", "--duration", "12", "--mode", "connect",
                       "--report", str(report_path)]
            run = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=50)
            if run.returncode != 0:
                raise RuntimeError(f"stress failed ({run.returncode}): {run.stdout[-1500:]} {run.stderr[-500:]}")
        finally:
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=10)
                raise RuntimeError("server did not stop cleanly")
    if proc.returncode != 0:
        raise RuntimeError(f"server exited with {proc.returncode}; see {log_path}")
    report = json.loads(report_path.read_text(encoding="utf-8"))
    log = log_path.read_text(encoding="utf-8")
    windows = [tuple(map(float, match)) for match in SUMMARY.findall(log)]
    active = max(windows, key=lambda values: values[5]) if windows else None
    workers = {}
    for match in WORKER.findall(log):
        worker, *values = match
        workers[int(worker)] = tuple(map(float, values))
    return {
        "network_workers": count,
        "duration_seconds": report["duration_seconds"],
        "connected": report["connected_successfully"],
        "maximum_concurrent": report["maximum_concurrent_connected"],
        "chunks_received": report["chunks_received"],
        "origin_chunk_seconds_mean": report["origin_chunk_seconds_mean"],
        "connection_seconds_mean": report["connection_seconds_mean"],
        "active_100_tick_window": None if active is None else {
            "tps": active[1], "p95_mspt": active[2], "p99_mspt": active[3],
            "ticks_over_50ms": int(active[4]), "protocol_player_ticks": int(active[5]),
        },
        "worker_accepted": [int(workers[i][2]) for i in range(count)],
        "worker_packets": [int(workers[i][4]) for i in range(count)],
        "worker_bytes_in": [int(workers[i][5]) for i in range(count)],
        "worker_bytes_out": [int(workers[i][6]) for i in range(count)],
        "bytes_in_per_second_approx": round(sum(workers[i][5] for i in range(count)) /
                                             report["duration_seconds"], 1),
        "bytes_out_per_second_approx": round(sum(workers[i][6] for i in range(count)) /
                                              report["duration_seconds"], 1),
        "worker_task_wait_avg_ms": [workers[i][7] for i in range(count)],
        "worker_cpu_pct": [workers[i][8] for i in range(count)],
    }


def main():
    RESULTS.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    result_dir = RESULTS / f"network-benchmark-{stamp}"
    result_dir.mkdir()
    rows = []
    for count in (1, 2, 4):
        row = benchmark(count, result_dir)
        rows.append(row)
        print(json.dumps(row), flush=True)
    (result_dir / "comparison.json").write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    print(f"Benchmark reports: {result_dir}")


if __name__ == "__main__":
    main()
