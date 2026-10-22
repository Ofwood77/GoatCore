#!/usr/bin/env python3
"""Sequential, same-load network/tick benchmark on disposable flat worlds."""

import argparse
import json
import os
import re
import shutil
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
    r"p99_total=([\d.]+)ms.*?over_50ms=(\d+).*?\btasks=(\d+).*?proto_ticks=(\d+)"
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


def benchmark(count, result_dir, bots=50, duration=12, mode="connect", slow_ms=1000,
              server_binary=None, world_template=None):
    name = {1: "one", 2: "two", 4: "four"}[count]
    port = free_port()
    world = result_dir / f"world-{name}"
    if world_template:
        shutil.copytree(world_template, world)
    else:
        world.mkdir()
    report_path = result_dir / f"report-{name}.json"
    log_path = result_dir / f"server-{name}.log"
    env = os.environ.copy()
    env.update({
        "MC_SETUP_FILE": str(ROOT / f"tests/stress/network/{name}.conf"),
        "MC_WORLD_PATH": str(world),
        "MC_BIND_PORT": str(port),
        "MC_PERF": "1",
        "MC_PERF_SLOW_MS": str(slow_ms),
        "MC_PERF_SUMMARY_TICKS": "100",
    })
    with log_path.open("w", encoding="utf-8") as log_file:
        proc = subprocess.Popen([str(server_binary or ROOT / "mc_server")], cwd=ROOT, env=env,
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
                       "--bots", str(bots), "--duration", str(duration), "--mode", mode,
                       "--report", str(report_path)]
            if mode in ("blocks", "mixed", "fastblocks"):
                command += ["--allow-block-changes"]
            run = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                                 timeout=max(50, duration + bots * 0.1 + 30))
            if run.returncode != 0 and not report_path.is_file():
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
    active = max(windows, key=lambda values: values[6]) if windows else None
    # The final window often contains idle connected players after the action
    # phase. Report the highest-task window separately instead of hiding peaks.
    max_players = max((values[6] for values in windows), default=0)
    loaded = [values for values in windows if values[6] >= max_players * 0.9]
    peak_tasks = max(loaded, key=lambda values: values[5]) if loaded else None
    worst_active = max(loaded, key=lambda values: values[2]) if loaded else None
    workers = {}
    for match in WORKER.findall(log):
        worker, *values = match
        workers[int(worker)] = tuple(map(float, values))
    return {
        "network_workers": count,
        "initial_world": "copied" if world_template else "fresh",
        "mode": mode,
        "requested_bots": bots,
        "stress_exit_code": run.returncode,
        "duration_seconds": report["duration_seconds"],
        "connected": report["connected_successfully"],
        "unexpected_disconnects": report["unexpected_disconnects"],
        "maximum_concurrent": report["maximum_concurrent_connected"],
        "movements_sent": report["movements_sent"],
        "blocks_placed_verified": report["blocks_placed_verified"],
        "blocks_broken_verified": report["blocks_broken_verified"],
        "actions_failed": report["actions_failed"],
        "chunks_received": report["chunks_received"],
        "origin_chunk_seconds_mean": report["origin_chunk_seconds_mean"],
        "connection_seconds_mean": report["connection_seconds_mean"],
        "active_100_tick_window": None if active is None else {
            "tps": active[1], "p95_mspt": active[2], "p99_mspt": active[3],
            "ticks_over_50ms": int(active[4]), "tasks": int(active[5]),
            "protocol_player_ticks": int(active[6]),
        },
        "peak_task_100_tick_window": None if peak_tasks is None else {
            "tps": peak_tasks[1], "p95_mspt": peak_tasks[2], "p99_mspt": peak_tasks[3],
            "ticks_over_50ms": int(peak_tasks[4]), "tasks": int(peak_tasks[5]),
            "protocol_player_ticks": int(peak_tasks[6]),
        },
        "worst_active_100_tick_window": None if worst_active is None else {
            "tps": worst_active[1], "p95_mspt": worst_active[2], "p99_mspt": worst_active[3],
            "ticks_over_50ms": int(worst_active[4]), "tasks": int(worst_active[5]),
            "protocol_player_ticks": int(worst_active[6]),
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
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bots", type=int, default=50)
    parser.add_argument("--duration", type=float, default=12)
    parser.add_argument("--mode", choices=("connect", "movement", "blocks", "mixed", "fastblocks"), default="connect")
    parser.add_argument("--slow-ms", type=int, default=1000,
                        help="MC_PERF slow-tick logging threshold")
    parser.add_argument("--workers", type=int, nargs="+", choices=(1, 2, 4), default=(1, 2, 4))
    parser.add_argument("--server-binary", type=Path, default=ROOT / "mc_server")
    parser.add_argument("--world-template", type=Path,
                        help="copy this disposable world before every server run")
    args = parser.parse_args()
    if not 1 <= args.bots <= (50 if args.mode in ("blocks", "mixed", "fastblocks") else 1000):
        parser.error("bots outside the supported range for this mode")
    if args.duration <= 0 or args.slow_ms <= 0:
        parser.error("duration must be positive")
    if not args.server_binary.is_file():
        parser.error(f"server binary not found: {args.server_binary}")
    if args.world_template and not args.world_template.is_dir():
        parser.error(f"world template not found: {args.world_template}")
    RESULTS.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    result_dir = RESULTS / f"network-benchmark-{stamp}"
    result_dir.mkdir()
    rows = []
    for count in args.workers:
        row = benchmark(count, result_dir, args.bots, args.duration, args.mode,
                        args.slow_ms, args.server_binary.resolve(), args.world_template)
        rows.append(row)
        print(json.dumps(row), flush=True)
    (result_dir / "comparison.json").write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    print(f"Benchmark reports: {result_dir}")


if __name__ == "__main__":
    main()
