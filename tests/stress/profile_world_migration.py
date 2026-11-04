#!/usr/bin/env python3
"""S3C fixed-work offline benchmarks. Timings and counted passes are separate.

Run only with disposable copies. The supplied historical world is read only;
the runner creates its own source subsets and fresh independent destinations.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests'))
from test_world_migration import compare, snapshot
from native_storage_oracle import parse
from native_corpus import encode
from profile_tick import write_summary


def run(args):
    args.result_dir = args.result_dir.resolve()
    args.result_dir.mkdir(parents=True, exist_ok=False)
    args.source, args.tool, args.probe = args.source.resolve(), args.tool.resolve(), args.probe.resolve()
    original = snapshot(args.source)
    files = sorted((args.source / 'chunks').glob('*.bin'))
    assert len(files) >= max((n for n in args.counts if n <= 483), default=1)
    templates = {}
    for count in args.counts:
        source = args.result_dir / f'source-{count}'
        selected = {files[i * len(files) // count].name for i in range(count)} if count <= len(files) else set()
        def ignore(path, names):
            return [n for n in names if n not in selected] if Path(path) == args.source / 'chunks' else []
        shutil.copytree(args.source, source, ignore=ignore)
        kind = 'subset/copy of actual GoatCore historical runtime'
        if not selected:
            # Larger stress fixture: declared synthetic relocation of a BE-free
            # real chunk, never claimed as naturally generated or historical.
            raw = parse(zlib.decompress(files[0].read_bytes(), wbits=47))
            assert not raw[b'block_entities'][1][1]
            for i in range(count):
                cx, cz = i % 32 - 32, i // 32 - 32
                raw[b'xPos'] = (3, struct.pack('>i', cx)); raw[b'zPos'] = (3, struct.pack('>i', cz))
                (source / 'chunks' / f'c_{cx}_{cz}.bin').write_bytes(zlib.compress(encode(raw)))
            kind = 'synthetic relocated BE-free historical chunk, negative almost-full region'
        templates[count] = source
        (args.result_dir / f'provenance-{count}.json').write_text(json.dumps(dict(kind=kind, chunks=count), indent=2))
    rows = []
    signatures = {n: snapshot(p) for n, p in templates.items()}
    for count in args.counts:
        source = templates[count]
        for repetition in range(args.repetitions):
            folder = args.result_dir / f'{count}-r{repetition + 1}'
            folder.mkdir()
            destination = folder / 'world'
            reports = {}
            for mode in ('analyze', 'convert', 'verify'):
                cmd = [str(args.tool), '--' + mode, '--source', str(source), '--destination', str(destination), '--source-offline']
                with (folder / (mode + '.jsonl')).open('x') as log:
                    done = subprocess.run(cmd, stdout=log, stderr=subprocess.PIPE, text=True, timeout=240)
                (folder / (mode + '.stderr')).write_text(done.stderr)
                assert done.returncode == 0, done.stderr
                result = json.loads((folder / (mode + '.jsonl')).read_text().splitlines()[-1])
                assert result['accepted'] == count and result['complete']
                if mode == 'convert': assert result['converted'] == result['verified'] == count and result['durable_publication']
                if mode == 'verify': assert result['verified'] == count
                reports[mode] = result
            assert compare(source, destination) == count
            assert snapshot(source) == signatures[count]
            conversion = reports['convert']
            row = dict(chunks=count, repetition=repetition + 1,
                       chunks_per_second=count / (conversion['elapsed_ms'] / 1000),
                       reports=reports, independent_typed_nbt_and_blocks='passed',
                       source_unchanged=True, counted_pass=None)
            rows.append(row)
            (args.result_dir / 'comparison.json').write_text(json.dumps(rows, indent=2))
            print(json.dumps(dict(chunks=count, repetition=repetition + 1,
                                  elapsed_ms=conversion['elapsed_ms'], verified=count)), flush=True)
    # Only after ALL ordinary timings: replay equivalent work with observer.
    for count in args.counts:
        folder = args.result_dir / f'{count}-counted'; folder.mkdir()
        source, destination = templates[count], folder / 'world'
        env = dict(os.environ, LD_PRELOAD=str(args.probe), MC_IO_TRACE_PATH=str(folder / 'writes.csv'),
                   MC_IO_TRACE_ROOT=str(folder), MC_FSYNC_TRACE_PATH=str(folder / 'fsync.csv'),
                   MC_PROC_FINAL_PATH=str(folder / 'process.json'), MC_SAVE_TRACE_PATH=str(folder / 'phases.csv'))
        with (folder / 'convert.jsonl').open('x') as log:
            done = subprocess.run([str(args.tool), '--convert', '--source', str(source), '--destination', str(destination),
                                   '--source-offline'], stdout=log, stderr=subprocess.PIPE, env=env, text=True, timeout=240)
        assert done.returncode == 0, done.stderr
        result = json.loads((folder / 'convert.jsonl').read_text().splitlines()[-1])
        assert result['converted'] == result['verified'] == count and result['durable_publication']
        assert compare(source, destination) == count and snapshot(source) == signatures[count]
        writes = write_summary(folder / 'writes.csv', 0, 2**63 - 1)
        final = json.loads((folder / 'process.json').read_text())
        assert final['world_write_calls'] == writes['calls']
        assert final['world_write_bytes'] == writes['completed_bytes'] and final['write_trace_dropped'] == 0
        with (folder / 'fsync.csv').open() as file: syncs = list(csv.reader(file))
        assert all(int(r[4]) == 0 for r in syncs)
        detail = dict(writes=writes, process=final, fsync_calls=len(syncs),
                      fsync_wall_ms=sum((int(r[1]) - int(r[0])) / 1000 for r in syncs),
                      note='Separate counted replay; elapsed time excluded from ranking')
        (folder / 'counts.json').write_text(json.dumps(detail, indent=2))
        for row in rows:
            if row['chunks'] == count: row['counted_pass'] = str(folder.relative_to(args.result_dir))
    assert snapshot(args.source) == original
    (args.result_dir / 'comparison.json').write_text(json.dumps(rows, indent=2))
    (args.result_dir / 'manifest.json').write_text(json.dumps(dict(
        tool_sha256=hashlib.sha256(args.tool.read_bytes()).hexdigest(),
        probe_sha256=hashlib.sha256(args.probe.read_bytes()).hexdigest(),
        timed_runs=len(rows), counted_runs=len(args.counts),
        hardware=args.hardware_label, no_tps_claim=True, source_unchanged=True), indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('tool', 'source', 'probe', 'result-dir'): parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--counts', nargs='+', type=int, choices=(1, 100, 483, 1000), default=[1, 100, 483, 1000])
    parser.add_argument('--repetitions', type=int, default=3)
    parser.add_argument('--hardware-label', default='Unspecified; record host/storage evidence separately',
                        help='explicit environment description, never inferred from logical I/O counts')
    args = parser.parse_args(); assert 1 <= args.repetitions <= 10
    run(args)
