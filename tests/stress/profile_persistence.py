#!/usr/bin/env python3
"""Remote-only persistence experiments. Never targets an existing service.

Backend durations run without ptrace. Separate traced repetitions count syscalls
and deliberately kill a child at a file syscall; their durations are not used
for performance comparisons. Original crash files are preserved before recovery.
"""
import argparse
import asyncio
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import time
from types import SimpleNamespace

from profile_tick import quantile
from test_block_placement import Placer, command
from test_block_persistence import query_block
from test_player_visibility import close

ROOT = Path(__file__).resolve().parents[2]


def invoke(argv, folder, env=None, expected=0):
    folder.mkdir(parents=True, exist_ok=False)
    (folder/'command.json').write_text(json.dumps(list(map(str, argv)), indent=2))
    with (folder/'stdout.log').open('w') as out, (folder/'stderr.log').open('w') as err:
        result = subprocess.run(list(map(str, argv)), env=env, stdout=out, stderr=err, timeout=240)
    assert result.returncode == expected, (folder, result.returncode, expected)
    return result


def markers(folder):
    return [json.loads(line) for line in (folder/'stdout.log').read_text().splitlines()
            if line.startswith('{"phase":')]


def phase_stats(folder):
    lo, hi = markers(folder)
    with (folder/'saves.csv').open() as file:
        rows = [r for r in csv.DictReader(file) if lo['us'] <= int(r['start_us']) < hi['us']]
    result = dict(duration_us=hi['duration_us'], writes=hi['writes'], phases={})
    for phase in sorted({r['phase'] for r in rows}):
        group = [r for r in rows if r['phase'] == phase]
        times = [int(r['end_us'])-int(r['start_us']) for r in group]
        result['phases'][phase] = dict(count=len(group), wall_us=sum(times),
            cpu_us=sum(int(r['cpu_us']) for r in group), bytes=sum(int(r['bytes']) for r in group),
            p50_us=quantile(times, .5), p95_us=quantile(times, .95),
            p99_us=quantile(times, .99), max_us=max(times), errors=sum(int(r['result']) != 0 for r in group))
    return result


def syscall_stats(folder):
    lo, hi = markers(folder)
    with (folder/'syscalls.csv').open() as file:
        rows = [r for r in csv.DictReader(file) if lo['us'] <= int(r['start_us']) < hi['us']]
    result = {}
    for call in sorted({r['call'] for r in rows}):
        group = [r for r in rows if r['call'] == call]
        result[call] = dict(count=len(group), bytes=sum(max(0, int(r['result'])) for r in group)
                           if call in ('read', 'write', 'pread', 'pwrite', 'writev') else 0,
                           errors=sum(int(r['result']) < 0 for r in group))
    return result


def backend(args, binaries):
    cases = [('idle', 40, 16), ('single', 200, 1), ('same', 200, 16), ('multi', 200, 16),
             ('region', 100, 1), ('region', 100, 16), ('region', 100, 256)]
    reports = []
    for mode, ops, count in cases:
        for repetition in range(args.repetitions):
            for label, binary in (binaries if repetition % 2 == 0 else list(reversed(binaries))):
                folder = args.result_dir/f'{label}-{mode}-{count}-r{repetition+1}'
                env = dict(os.environ, MC_SAVE_TRACE_PATH=str(folder/'saves.csv'))
                invoke([binary, mode, folder/'world', ops, count], folder, env)
                row = dict(label=label, mode=mode, count=count, repetition=repetition+1,
                           sha256=hashlib.sha256(binary.read_bytes()).hexdigest(), **phase_stats(folder))
                reports.append(row)
                (folder/'summary.json').write_text(json.dumps(row, indent=2))
                print(json.dumps(row), flush=True)
                # Counts in a separate pass, with the same number of operations.
                if repetition == 0:
                    traced = args.result_dir/f'{label}-{mode}-{count}-syscalls'
                    argv = [args.tracer, traced/'syscalls.csv', traced/'world', '', 'none', '--',
                            binary, mode, traced/'world', ops, count]
                    # The trace file needs its parent before the tool starts.
                    traced.mkdir()
                    with (traced/'stdout.log').open('w') as out, (traced/'stderr.log').open('w') as err:
                        done = subprocess.run(list(map(str, argv)), stdout=out, stderr=err, timeout=240)
                    assert done.returncode == 0, traced
                    (traced/'command.json').write_text(json.dumps(list(map(str, argv)), indent=2))
                    (traced/'summary.json').write_text(json.dumps(syscall_stats(traced), indent=2))
    (args.result_dir/'comparison.json').write_text(json.dumps(reports, indent=2))
    # Deterministic compressed output must also remain byte-identical.
    assert len(binaries) == 2, 'byte equivalence requires the reference and candidate'
    for mode, _, count in cases:
        if mode == 'region': continue  # region header timestamps are wall-clock based
        a = args.result_dir/f'{binaries[0][0]}-{mode}-{count}-r1/world/chunks'
        b = args.result_dir/f'{binaries[1][0]}-{mode}-{count}-r1/world/chunks'
        assert {p.name: p.read_bytes() for p in a.iterdir()} == {p.name: p.read_bytes() for p in b.iterdir()}


def inspect(binary, world, recover=False):
    done = subprocess.run([str(binary), 'recover' if recover else 'inspect', str(world), '1', '1'],
                          capture_output=True, text=True, timeout=60, check=True)
    return json.loads(next(line for line in done.stdout.splitlines() if line.startswith('{')))


async def crash_server(args, label, server, seed, bench):
    results = []
    stages = ('temp-create-before', 'temp-write-partial', 'temp-sync-before', 'rename-before',
              'dir-sync-before', 'dir-sync-after') if args.atomic else (
              'open-before', 'open-truncate', 'write-before', 'write-partial', 'write-after', 'close-after')
    for stage in stages:
        folder = args.result_dir/f'{label}-{stage}'
        runtime = folder/'runtime'
        runtime.mkdir(parents=True)
        world = runtime/'world'
        shutil.copytree(seed, world)
        target = world/'chunks/c_-32_-32.bin'
        before = target.read_bytes()
        (runtime/'assets').symlink_to(ROOT/'assets')
        (runtime/'server.config').write_text('online-mode=false\noffline-skins=false\nview-distance=2\n')
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]
        assert port not in (25565, 25587)
        env = dict(os.environ, MC_BIND_PORT=str(port), MC_WORLD_PATH=str(world), MC_QA_MODE='1',
                   MC_SETUP_FILE=str(ROOT/'tests/stress/network/one.conf'), MC_ADMIN_USERNAME='Ofwood_')
        argv = list(map(str, [args.tracer, folder/'syscalls.csv', world, target, stage, '--', server]))
        (folder/'command.json').write_text(json.dumps(argv, indent=2))
        opts = SimpleNamespace(host='127.0.0.1', port=port, timeout=60, run_id='CRSH', shared_zone=False,
                               mode='connect', qa_verify_movement=False)
        player = Placer(0, opts); player.name = 'Ofwood_'
        with (folder/'server.log').open('w') as log:
            proc = subprocess.Popen(argv, cwd=runtime, env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic()+90
                while True:
                    if proc.poll() is not None: raise RuntimeError(('startup', folder, proc.returncode))
                    try:
                        with socket.create_connection(('127.0.0.1', port), timeout=.2): break
                    except ConnectionRefusedError:
                        if time.monotonic() > deadline: raise TimeoutError('crash server readiness')
                        await asyncio.sleep(.05)
                await player.connect(); await player.conn.send(44)
                assert await query_block(player, (-512, 64, -497)) == args.stone
                assert await query_block(player, (-511, 64, -497)) == args.log
                await command(player, 'setblock -512 64 -497 minecraft:oak_log[axis=x]')
                await asyncio.to_thread(proc.wait, 90)
                assert proc.returncode == 137, (folder, proc.returncode)
            finally:
                if proc.poll() is None:
                    proc.terminate()  # PTRACE_O_EXITKILL affects only this launcher's child
                    await asyncio.to_thread(proc.wait, 10)
                if player.conn:
                    try: await close(player)
                    except (ConnectionError, asyncio.TimeoutError): pass
        # Preserve evidence before the world loader can regenerate a bad chunk.
        snapshot = target.read_bytes()
        (folder/'chunk-after.bin').write_bytes(snapshot)
        decoded = inspect(bench, world)
        recovered = folder/'recovered-world'
        shutil.copytree(world, recovered)
        recovery = inspect(bench, recovered, True)
        row = dict(label=label, stage=stage, old_bytes=len(before), new_bytes=len(snapshot),
                   old_sha256=hashlib.sha256(before).hexdigest(), new_sha256=hashlib.sha256(snapshot).hexdigest(),
                   decoded=decoded, recovery=recovery, server_returncode=proc.returncode)
        if args.atomic:
            expected = args.log if stage in ('dir-sync-before','dir-sync-after') else args.stone
            assert decoded['read_rc']==recovery['read_rc']==0
            assert decoded['state']==recovery['state']==expected
            assert decoded['sentinel']==recovery['sentinel']==args.log
            if expected==args.stone: assert snapshot==before
        if stage == 'open-before': assert snapshot == before and decoded['read_rc'] == 0
        if stage in ('open-truncate', 'write-before', 'write-partial'): assert decoded['read_rc'] == -1
        if stage in ('write-after', 'close-after'): assert decoded['read_rc'] == 0 and decoded['state'] == args.log
        results.append(row); (folder/'summary.json').write_text(json.dumps(row, indent=2))
        print(json.dumps(row), flush=True)
    return results


async def crashes(args, binaries):
    assert args.server and len(args.server) == len(binaries)
    # Four updates end on stone, with an adjacent x-axis log as a loss sentinel.
    seed_run = args.result_dir/'seed'
    bench = binaries[-1][1]
    invoke([bench, 'single', seed_run/'world', 4, 1], seed_run)
    report = inspect(bench, seed_run/'world')
    args.stone, args.log = report['state'], report['sentinel']
    assert args.stone and args.log and args.stone != args.log
    results = []
    for entry in args.server:
        label, path = entry.split('=', 1)
        assert label in dict(binaries)
        results += await crash_server(args, label, Path(path).resolve(), seed_run/'world', bench)
    (args.result_dir/'crashes.json').write_text(json.dumps(results, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', action='append', required=True, help='label=backend benchmark binary')
    parser.add_argument('--tracer', type=Path, default=ROOT/'mc_trace_io')
    parser.add_argument('--result-dir', type=Path, required=True)
    parser.add_argument('--repetitions', type=int, default=3)
    parser.add_argument('--crash', action='store_true')
    parser.add_argument('--atomic', action='store_true', help='interrupt temp/fsync/rename and require preserved old or new data')
    parser.add_argument('--server', action='append', help='label=server binary, only for isolated crash experiments')
    args = parser.parse_args()
    assert args.repetitions >= 1
    args.result_dir = args.result_dir.resolve(); args.tracer = args.tracer.resolve()
    assert args.result_dir.is_relative_to(ROOT/'tests/stress/results'), 'use the isolated test result tree'
    args.result_dir.mkdir(parents=True, exist_ok=False)
    binaries = [(label, Path(path).resolve()) for label, path in (entry.split('=', 1) for entry in args.binary)]
    assert all(label and all(c.isalnum() or c in '-_' for c in label) and path.is_file() for label, path in binaries)
    if args.crash: asyncio.run(crashes(args, binaries))
    else: backend(args, binaries)
