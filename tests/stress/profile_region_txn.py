#!/usr/bin/env python3
"""Fixed-work experimental Anvil comparison; never launches a Minecraft server.

Creates fresh disposable roots only. Separate ptrace passes count I/O; their
wall times are excluded. Recovery runs on original inodes before any export.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
from check_region_storage import reference
from profile_tick import quantile


def invoke(command, folder, env=None, expected=0):
    folder.mkdir(parents=True, exist_ok=False)
    (folder/'command.json').write_text(json.dumps(list(map(str, command)), indent=2))
    with (folder/'stdout.log').open('w') as out, (folder/'stderr.log').open('w') as err:
        result = subprocess.run(list(map(str, command)), stdout=out, stderr=err, env=env, timeout=300)
    assert result.returncode == expected, (folder, result.returncode, expected)
    return result


def snapshot(root):
    result = {}
    for path in sorted(root.glob('*.mca')):
        entries, used, decoded = reference(path)
        assert len(entries) == len(decoded), path
        free_runs = sum(not busy and (i == 0 or used[i-1]) for i, busy in enumerate(used))
        run = largest = 0
        for busy in used:
            run = 0 if busy else run + 1
            largest = max(largest, run)
        result[path.name] = dict(chunks=len(entries), sectors=len(used), free=len(used)-sum(used),
                                 free_runs=free_runs, largest_free_run=largest,
                                 nbt={str(slot): hashlib.sha256(raw).hexdigest() for slot, raw in decoded.items()})
    assert result
    return result


def summarize(folder):
    records = [json.loads(line) for line in (folder/'stdout.log').read_text().splitlines() if line.startswith('{')]
    start = next(r for r in records if r.get('phase') == 'begin')
    end = next(r for r in records if r.get('phase') == 'end')
    actions = [r for r in records if 'action' in r]
    assert len(actions) == end['confirmed'] and [a['action'] for a in actions] == list(range(end['confirmed']))
    times = [a['wall_ns'] for a in actions]
    summary = dict(duration_ns=end['duration_ns'], cpu_ns=end['cpu_ns'],
                   non_cpu_ns=max(0, end['duration_ns']-end['cpu_ns']), serialize_ns=end['serialize_ns'],
                   confirmed=end['confirmed'], p50_ns=quantile(times, .5), p95_ns=quantile(times, .95),
                   p99_ns=quantile(times, .99), max_ns=max(times), regions=snapshot(folder/'world'), phases={})
    phases = folder/'phases.csv'
    if phases.exists():
        rows = [r for r in csv.DictReader(phases.open()) if start['us'] <= int(r['start_us']) < end['us']]
        for phase in sorted({r['phase'] for r in rows}):
            group = [r for r in rows if r['phase'] == phase]
            durations = [int(r['end_us'])-int(r['start_us']) for r in group]
            summary['phases'][phase] = dict(count=len(group), wall_us=sum(durations),
                cpu_us=sum(int(r['cpu_us']) for r in group), bytes=sum(int(r['bytes']) for r in group),
                p50_us=quantile(durations, .5), p95_us=quantile(durations, .95), p99_us=quantile(durations, .99))
    traced = folder/'syscalls.csv'
    if traced.exists():
        rows = [r for r in csv.DictReader(traced.open()) if start['us'] <= int(r['start_us']) < end['us']]
        summary['syscalls'] = {}
        for name in sorted({r['call'] for r in rows}):
            group = [r for r in rows if r['call'] == name]
            summary['syscalls'][name] = dict(count=len(group), errors=sum(int(r['result']) < 0 for r in group),
                bytes=sum(max(0, int(r['result'])) for r in group) if name in ('write', 'pwrite') else 0)
        summary['write_bytes'] = sum(v['bytes'] for v in summary['syscalls'].values())
        summary['region_write_bytes'] = sum(max(0, int(r['result'])) for r in rows
             if r['call'] in ('write', 'pwrite') and (r['path'].endswith('.mca') or '.mca.tmp.' in r['path']))
    (folder/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    return summary


def recover_case(args, world, folder, stage):
    before = snapshot(world)
    region = world/'r.-1.-1.mca'
    invoke([args.fault_tool, '--experimental-disposable', 'crash-region', region, 0, 0, stage], folder/'interrupt', expected=-9)
    journal = region.with_name(region.name+'.s2-journal')
    assert journal.is_file()
    shutil.copy2(journal, folder/'journal-before-recovery.bin')
    for step in ['recover', 'recover-again']:
        invoke([args.tool, '--experimental-disposable', 'recover', world], folder/step)
    assert not list(world.glob('*.s2-journal'))
    after = snapshot(world)
    assert {k:v['nbt'] for k,v in before.items()} == {k:v['nbt'] for k,v in after.items()}
    result = json.loads((folder/'recover/stdout.log').read_text())
    result.update(stage=stage, journal_bytes=journal.stat().st_size if journal.exists() else (folder/'journal-before-recovery.bin').stat().st_size,
                  repeated=json.loads((folder/'recover-again/stdout.log').read_text()), nbt_equal=True)
    (folder/'summary.json').write_text(json.dumps(result, indent=2)+'\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tool', type=Path, required=True)
    parser.add_argument('--fault-tool', type=Path, required=True)
    parser.add_argument('--tracer', type=Path, required=True)
    parser.add_argument('--result-dir', type=Path, required=True)
    parser.add_argument('--repetitions', type=int, default=3)
    parser.add_argument('--operations', type=int, default=100)
    parser.add_argument('--counts', type=int, nargs='+', default=[1, 256, 1024], choices=[1, 256, 1024])
    parser.add_argument('--patterns', nargs='+', default=['single', 'spread', 'regions'], choices=['single', 'spread', 'regions'])
    args = parser.parse_args()
    assert args.repetitions > 0 and args.operations > 0
    args.tool = args.tool.resolve(); args.fault_tool = args.fault_tool.resolve(); args.tracer = args.tracer.resolve()
    args.result_dir = args.result_dir.resolve(); args.result_dir.mkdir(parents=True, exist_ok=False)
    reports = []
    for count in args.counts:
        for pattern in args.patterns:
            for rep in range(args.repetitions):
                pair = []
                for backend in (['atomic', 's2'] if rep % 2 == 0 else ['s2', 'atomic']):
                    folder = args.result_dir/f'{backend}-{count}-{pattern}-r{rep+1}'
                    env = dict(os.environ, MC_SAVE_TRACE_PATH=str(folder/'phases.csv'))
                    command = [args.tool, '--experimental-disposable', 'bench', folder/'world', backend, count, pattern, args.operations]
                    invoke(command, folder, env)
                    row = dict(backend=backend, count=count, pattern=pattern, repetition=rep+1, **summarize(folder))
                    if rep == 0:
                        traced = args.result_dir/f'{backend}-{count}-{pattern}-traced'
                        cmd = [args.tracer, traced/'syscalls.csv', traced/'world', '', 'none', '--', args.tool,
                               '--experimental-disposable', 'bench', traced/'world', backend, count, pattern, args.operations]
                        invoke(cmd, traced)
                        row['io'] = summarize(traced)['syscalls']
                        row['write_bytes'] = json.loads((traced/'summary.json').read_text())['write_bytes']
                    pair.append(row); reports.append(row)
                    print(json.dumps({k:row[k] for k in ['backend', 'count', 'pattern', 'repetition', 'duration_ns', 'cpu_ns', 'confirmed']}), flush=True)
                assert {k:v['nbt'] for k,v in pair[0]['regions'].items()} == {k:v['nbt'] for k,v in pair[1]['regions'].items()}
    # No recovery preparation or interrupted run overlaps any timed write pass.
    recovery = []
    for row in reports:
        if row['backend'] != 's2':
            continue
        name = f"s2-{row['count']}-{row['pattern']}-r{row['repetition']}"
        stage = 'header_partial' if row['repetition'] % 2 else 'transaction_committed'
        recovered = recover_case(args, args.result_dir/name/'world', args.result_dir/(name+'-recovery'), stage)
        recovery.append(dict(count=row['count'], pattern=row['pattern'], repetition=row['repetition'], **recovered))
    (args.result_dir/'comparison.json').write_text(json.dumps(reports, indent=2)+'\n')
    (args.result_dir/'recovery.json').write_text(json.dumps(recovery, indent=2)+'\n')
    manifest = dict(kernel=platform.platform(), cpu=platform.processor(), python=platform.python_version(),
                    hardware_note='Local NVMe; no TPS claim, no Xeon/HDD equivalence',
                    binaries={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in [args.tool, args.fault_tool, args.tracer]})
    (args.result_dir/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print(json.dumps(dict(passes=len(reports), traced=2*len(args.counts)*len(args.patterns), recoveries=len(recovery), result='passed')))


if __name__ == '__main__':
    main()
