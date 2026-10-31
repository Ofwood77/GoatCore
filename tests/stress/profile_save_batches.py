#!/usr/bin/env python3
"""S4 fixed-work region benchmarks, explicitly marked disposable worlds only.

Serial publishes each intermediate mutation. Grouped publishes the final states
with bounded batches of 64 snapshots. Both acknowledge final durability before
returning; intermediate durability differs by design, never reported as equal.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
from native_storage_oracle import preservation, same_blocks
from profile_tick import quantile


def read_jsonl(path):
    return [json.loads(line) for line in path.read_text().splitlines()]


def publication_window(phases):
    """Exclude backend-marker initialization and readback from write counts."""
    publications = [r for r in phases if r['phase'] == 'region_total']
    assert publications and all(int(r['result']) == 0 for r in publications)
    start = min(int(r['start_us']) for r in publications)
    end = max(int(r['end_us']) for r in publications)
    return [r for r in phases if start <= int(r['start_us']) and int(r['end_us']) <= end]


def run(args):
    result = args.result_dir.resolve()
    result.mkdir(parents=True, exist_ok=False)
    definitions = [
        ('empty', 0, 1, 1, 'absent'),
        ('256-same', 256, 256, 100, 'same'),
        ('256-spread', 256, 256, 100, 'spread'),
        ('1024-same', 1024, 1024, 100, 'same'),
        ('1024-spread', 1024, 1024, 100, 'spread'),
        ('regions', 1, 3, 100, 'regions'),
        ('opaque-be', None, 1, 100, 'same'),
        ('dense-be', None, 1, 100, 'same'),
        ('opaque-fields', None, 1, 100, 'same'),
    ]
    runs = []
    for name, occupancy, count, changes, distribution in definitions:
        if args.cases and name not in args.cases:
            continue
        fixture = args.dense_world if name=='dense-be' else args.opaque_world if name=='opaque-fields' else args.fixture_world
        if occupancy is None and fixture is None:continue
        source = result / ('seed-' + name)
        if occupancy is None:
            shutil.copytree(fixture.resolve(), source)
        else:
            source.mkdir()
        (source / '.goatcore-s3b-disposable').write_text('S4 disposable test only\n')
        if occupancy is not None:
            for cx in ([0, 32, 64] if distribution == 'regions' else [0]):
                subprocess.run([str(args.probe.resolve()), '--seed-region', str(source),
                                str(cx), '0', str(occupancy)], check=True,
                               stdout=subprocess.DEVNULL)
        manifest = {str(p.relative_to(source)): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in source.rglob('*') if p.is_file()}
        (result / (name + '-seed.json')).write_text(json.dumps(manifest, indent=2))
        variants = [('S3B-serial', args.baseline_probe, 'serial'),
                    ('S4-serial', args.probe, 'serial'), ('S4-grouped', args.probe, 'grouped')]
        for rep in range(args.repetitions):
            final_world = None
            order = variants if rep % 2 == 0 else list(reversed(variants))
            for label, probe, policy in order:
                folder = result / f'{name}-{label}-r{rep+1}'
                folder.mkdir()
                world = folder / 'world'
                shutil.copytree(source, world)
                env = dict(os.environ, MC_SAVE_TRACE_PATH=str(folder/'saves.csv'))
                if args.fsync_probe:
                    env.update(LD_PRELOAD=str(args.fsync_probe.resolve()),
                               MC_FSYNC_TRACE_PATH=str(folder/'fsync.csv'))
                cx, cz = (-1, 1) if occupancy is None else (0, 0)
                process = subprocess.run([str(probe.resolve()), '--batch-bench', str(world),
                                          str(cx), str(cz), str(count), str(changes), distribution, policy],
                                         capture_output=True, text=True, env=env, timeout=180)
                (folder/'stdout.jsonl').write_text(process.stdout)
                (folder/'stderr.log').write_text(process.stderr)
                assert process.returncode == 0, (name, label, process.stderr)
                records = read_jsonl(folder/'stdout.jsonl')
                confirmation = next(r for r in records if 'readback' in r)
                assert confirmation['readback'] and confirmation['changes'] == changes
                phases = list(csv.DictReader((folder/'saves.csv').open()))
                publications = [r for r in phases if r['phase'] == 'region_total']
                assert publications and all(int(r['result']) == 0 for r in publications)
                write_phases = publication_window(phases)
                if final_world is not None:
                    proof = same_blocks(final_world, world)
                    (folder/'same-blocks.json').write_text(json.dumps(proof, indent=2))
                else:
                    final_world = world
                if distribution != 'absent':
                    proof = preservation(source, world)
                    (folder/'preservation.json').write_text(json.dumps(proof, indent=2))
                work = next(r for r in records if r.get('phase') == 'work_total')
                summary = dict(case=name, label=label, repetition=rep+1,
                               sha256=hashlib.sha256(probe.read_bytes()).hexdigest(),
                               changes=changes, saved_chunk_versions=confirmation['saved_chunks'],
                               unique_dirty_chunks=confirmation['unique_dirty_chunks'],
                               readback_chunks=confirmation['readback_chunks'],
                               wall_ms=work['ns']/1e6, cpu_ms=work['cpu_ns']/1e6,
                               rss_kb=confirmation['rss_kb'],
                               reconstructions=len(publications),
                               written_bytes=sum(int(r['bytes']) for r in publications),
                               fsync_file=sum(r['phase'] == 'atomic_file_sync' for r in write_phases),
                               fsync_parent=sum(r['phase'] == 'atomic_directory_sync' for r in write_phases),
                               load_ms=next(r['ns']/1e6 for r in records if r.get('phase')=='load_all'))
                assert summary['fsync_file'] == summary['fsync_parent'] == len(publications)
                runs.append(summary)
                (result/'runs.json').write_text(json.dumps(runs, indent=2))
                print(json.dumps(summary), flush=True)
    groups = {}
    for row in runs:
        groups.setdefault(row['case']+'/'+row['label'], []).append(row)
    summary = {}
    for key, rows in groups.items():
        wall = [r['wall_ms'] for r in rows]
        summary[key] = dict(repetitions=len(rows), median_ms=quantile(wall, .5),
                            min_ms=min(wall), max_ms=max(wall),
                            reconstructions=[r['reconstructions'] for r in rows],
                            bytes=[r['written_bytes'] for r in rows],
                            fsync=[r['fsync_file']+r['fsync_parent'] for r in rows],
                            cpu_ms=[r['cpu_ms'] for r in rows], rss_kb=[r['rss_kb'] for r in rows])
    (result/'summary.json').write_text(json.dumps(summary, indent=2))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    for option in ('probe', 'baseline-probe', 'fixture-world', 'result-dir'):
        p.add_argument('--'+option, type=Path, required=True)
    p.add_argument('--dense-world', type=Path)
    p.add_argument('--opaque-world', type=Path)
    p.add_argument('--fsync-probe', type=Path)
    p.add_argument('--repetitions', type=int, default=3)
    p.add_argument('--cases', nargs='+')
    args = p.parse_args()
    assert 1 <= args.repetitions <= 10
    run(args)
