#!/usr/bin/env python3
import argparse
import re
from pathlib import Path


TOKEN_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=([^ ]+)")


def parse_perf_line(line):
    if "perf " not in line:
        return None
    marker = line.split("perf ", 1)[1].strip()
    if ":" not in marker:
        return None
    kind, rest = marker.split(":", 1)
    values = {}
    for key, raw in TOKEN_RE.findall(rest):
        if raw.endswith("ms"):
            raw = raw[:-2]
        try:
            values[key] = float(raw)
        except ValueError:
            values[key] = raw
    return kind.strip(), values, line.rstrip("\n")


def top_by(rows, key, limit):
    return sorted((r for r in rows if isinstance(r[1].get(key), float)), key=lambda r: r[1][key], reverse=True)[:limit]


def print_rows(title, rows, keys):
    print(title)
    if not rows:
        print("  (none)")
        return
    for kind, values, _line in rows:
        parts = []
        for key in keys:
            value = values.get(key)
            if isinstance(value, float):
                parts.append(f"{key}={value:.3f}ms" if key not in ("idx", "sent", "scans", "misses") else f"{key}={value:g}")
            elif value is not None:
                parts.append(f"{key}={value}")
        print(f"  {kind}: " + " ".join(parts))


def main():
    parser = argparse.ArgumentParser(description="Summarize MC_PERF logs from mc_c_server.")
    parser.add_argument("logfile", type=Path)
    parser.add_argument("--top", type=int, default=20)
    args = parser.parse_args()

    rows = []
    with args.logfile.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            parsed = parse_perf_line(line)
            if parsed:
                rows.append(parsed)

    ticks = [r for r in rows if r[0] == "tick"]
    chunks = [r for r in rows if r[0] == "chunk_ready"]
    streams = [r for r in rows if r[0] == "chunk_stream"]
    summaries = [r for r in rows if r[0] == "summary"]
    furnaces = [r for r in rows if r[0] == "furnaces"]

    print(f"parsed perf rows: total={len(rows)} ticks={len(ticks)} chunks={len(chunks)} streams={len(streams)} summaries={len(summaries)} furnaces={len(furnaces)}")
    print_rows("top ticks by total", top_by(ticks, "total", args.top), ["idx", "total", "late", "tasks", "world", "furnaces", "updates", "proto", "items", "remote", "write", "evict"])
    print_rows("top ticks by late", top_by(ticks, "late", args.top), ["idx", "late", "total", "task_wait_max", "tasks", "world", "write"])
    print_rows("top chunks by elapsed", top_by(chunks, "elapsed", args.top), ["chunk", "elapsed", "encode", "heightmap", "block_entities_scan", "payload_build", "write_queue", "payload", "chunkdata"])
    print_rows("top chunk streams by elapsed", top_by(streams, "elapsed", args.top), ["player", "elapsed", "sent", "scans", "misses", "pending_start", "pending_end", "view", "center"])
    print_rows("top furnace scans by elapsed", top_by(furnaces, "elapsed", args.top), ["elapsed", "scanned", "machines", "open_skipped", "ticked", "changed", "errors"])


if __name__ == "__main__":
    main()
