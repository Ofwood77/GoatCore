#!/usr/bin/env python3
"""Independent read-only Anvil oracle and local S1 microbenchmarks.

No server is launched. Originals are hashed before/after. Output must be fresh.
The optional native regions should come from an isolated official 26.1.1 world.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import statistics
import struct
import subprocess
import tempfile
import zlib


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def reference(path):
    data = path.read_bytes()
    assert len(data) >= 8192 and len(data) % 4096 == 0
    used = [False] * (len(data) // 4096)
    used[0:2] = [True, True]
    entries, decoded = {}, {}
    for slot in range(1024):
        raw = struct.unpack_from('>I', data, 4 * slot)[0]
        if not raw:
            continue
        off, count = raw >> 8, raw & 255
        assert off >= 2 and count and off + count <= len(used)
        assert not any(used[off:off + count])
        used[off:off + count] = [True] * count
        n = struct.unpack_from('>I', data, off * 4096)[0]
        assert n >= 1 and n + 4 <= count * 4096
        codec = data[off * 4096 + 4]
        entries[slot] = dict(entry=slot, offset=off, count=count, length=n,
                             codec=codec, timestamp=struct.unpack_from('>I', data, 4096 + 4 * slot)[0])
        payload = data[off * 4096 + 5:off * 4096 + 4 + n]
        if not codec & 128 and codec in (1, 2, 3):
            decoded[slot] = gzip.decompress(payload) if codec == 1 else zlib.decompress(payload) if codec == 2 else payload
    return entries, used, decoded


def run(binary, path, out, reps, dump=False):
    before = digest(path)
    args = [str(binary), str(path), str(reps)]
    if dump:
        args.append(str(out / 'nbt'))
    proc = subprocess.run(args, capture_output=True, text=True)
    (out / 'stdout.jsonl').write_text(proc.stdout)
    (out / 'stderr.log').write_text(proc.stderr)
    assert proc.returncode == 0, proc.stderr
    records = [json.loads(line) for line in proc.stdout.splitlines()]
    entries, used, decoded = reference(path)
    actual = {r['entry']: r for r in records if 'entry' in r}
    assert actual == entries
    metadata = next(r for r in records if r.get('metadata'))
    assert metadata['chunks'] == len(entries)
    assert metadata['sectors'] == len(used)
    assert metadata['occupied'] == sum(used)
    assert metadata['free'] == len(used) - sum(used)
    assert metadata['external'] == sum(bool(e['codec'] & 128) for e in entries.values())
    assert metadata['unsupported'] == sum(not 1 <= (e['codec'] & 127) <= 3 for e in entries.values())
    assert next(r['used'] for r in records if 'used' in r) == ''.join('1' if s else '0' for s in used)
    if dump:
        assert len(list((out / 'nbt').glob('*.nbt'))) == len(decoded)
        for slot, data in decoded.items():
            assert (out / 'nbt' / f'{slot}.nbt').read_bytes() == data
    assert digest(path) == before
    samples = {}
    for r in records:
        if 'phase' in r:
            samples.setdefault(r['phase'], []).append(r['ns'])
    summary = dict(path=str(path), sha256=before, chunks=len(entries), decoded=len(decoded), metadata=metadata, phases={})
    for phase, values in samples.items():
        ordered = sorted(values)
        summary['phases'][phase] = dict(samples=len(values), p50_ns=statistics.median(values),
            p95_ns=ordered[min(len(ordered)-1, int(.95*len(ordered)))],
            p99_ns=ordered[min(len(ordered)-1, int(.99*len(ordered)))])
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    return summary


def synthetic(path):
    # Valid NBT with an uninterpreted string and an opaque byte array; all bytes
    # must survive read/decompression, not just supported world fields.
    nbt = b'\x0a\x00\x00\x08\x00\x07unknown\x00\x04S1!?\x07\x00\x04blob\x00\x00\x00\x04\x00\xff\x80\x01\x00'
    data = bytearray(14 * 4096)
    for slot, codec in enumerate((1, 2, 3, 4, 127, 130, 42)):
        off = 2 + slot * 1 + (slot >= 3) * 2  # two unused sectors plus trailing holes
        payload = gzip.compress(nbt, mtime=0) if codec == 1 else zlib.compress(nbt) if codec == 2 else nbt if codec == 3 else b''
        struct.pack_into('>I', data, slot * 4, off << 8 | 1)
        struct.pack_into('>I', data, 4096 + slot * 4, 0x12345678 + slot)
        struct.pack_into('>IB', data, off * 4096, len(payload) + 1, codec)
        data[off*4096+5:off*4096+5+len(payload)] = payload
    path.write_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=Path('./mc_region_bench'))
    parser.add_argument('--region-dir', type=Path)
    parser.add_argument('--result-dir', type=Path, required=True)
    parser.add_argument('--repetitions', type=int, default=30)
    args = parser.parse_args()
    args.result_dir.mkdir(parents=True, exist_ok=False)
    results = []
    fixtures = args.result_dir / 'synthetic.mca'
    synthetic(fixtures)
    out = args.result_dir / 'synthetic'; out.mkdir()
    results.append(run(args.binary.resolve(), fixtures, out, args.repetitions, dump=True))
    if args.region_dir:
        for path in sorted(args.region_dir.glob('*.mca')):
            out = args.result_dir / path.stem; out.mkdir()
            results.append(run(args.binary.resolve(), path, out, args.repetitions, dump=True))
    (args.result_dir / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps(dict(regions=len(results), chunks=sum(r['chunks'] for r in results),
                          decoded=sum(r['decoded'] for r in results), source_hashes_unchanged=True)))


if __name__ == '__main__':
    main()
