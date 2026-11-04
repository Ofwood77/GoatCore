#!/usr/bin/env python3
"""Offline migration CLI: independent NBT oracle, ownership and process faults.

Synthetic fixtures are explicitly labelled; no Minecraft server is started.
Every test owns a TemporaryDirectory. No real world or service is consulted.
"""
import argparse
import copy
import fcntl
import hashlib
import json
import os
import random
from pathlib import Path
import select
import signal
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent / 'stress'))
from native_corpus import encode
from native_storage_oracle import parse, value, sections, section_blocks, chunks

TOOL = FAULT = None
EVIDENCE = []
CRASH_PHASES = ('analysis', 'read-before', 'source-read-partial', 'convert', 'prepare',
                'region-write', 'region-write-partial', 'region-file-sync-before',
                'region-file-sync-after', 'verify', 'verify-read',
                'files-sync-before', 'files-sync-after', 'publish-before', 'publish-after', 'durable')


def integer(n):
    return (3, struct.pack('>i', n))


def packed(indices, bits):
    words = [0] * ((len(indices) * bits + 63) // 64)
    for i, index in enumerate(indices):
        at, shift = divmod(i * bits, 64)
        words[at] |= index << shift
        if shift + bits > 64:
            words[at + 1] |= index >> (64 - shift)
    return b''.join(struct.pack('>Q', word & ((1 << 64) - 1)) for word in words)


def fixture(cx=0, cz=0, version=4788):
    # Generated independently of GoatCore's writer; simple two-state section.
    palette = [{b'Name': (8, name)} for name in (b'minecraft:stone', b'minecraft:dirt')]
    palette[0][b'FuturePaletteField'] = (4, struct.pack('>q', -775))
    states = {b'palette': (9, (10, palette)), b'data': (12, packed([i % 2 for i in range(4096)], 4))}
    states[b'FutureContainerField'] = (7, b'opaque')
    return {b'xPos': integer(cx), b'zPos': integer(cz), b'yPos': integer(-4),
            b'DataVersion': integer(version), b'Status': (8, b'minecraft:full'),
            b'sections': (9, (10, [{b'Y': (1, b'\x04'), b'block_states': (10, states),
                                   b'biomes': (10, {b'palette': (9, (8, [b'minecraft:plains']))})}])),
            b'block_entities': (9, (10, [])),
            b'GoatCoreOpaque': (10, {b'number': (4, struct.pack('>q', -7754788)),
                                      b'bytes': (7, bytes(range(256))),
                                      b'list': (9, (10, [{b'signed': (1, b'\xef')}]))})}


def write_chunk(world, root, name=None, gzip=False):
    cx = int.from_bytes(value(root, 'xPos'), 'big', signed=True)
    cz = int.from_bytes(value(root, 'zPos'), 'big', signed=True)
    path = world / 'chunks' / (name or f'c_{cx}_{cz}.bin')
    path.parent.mkdir(parents=True, exist_ok=True)
    raw = encode(root)
    if gzip:
        compressor = zlib.compressobj(wbits=31)
        path.write_bytes(compressor.compress(raw) + compressor.flush())
    else:
        path.write_bytes(zlib.compress(raw))
    return path


def snapshot(world):
    result = {}
    for p in sorted(world.rglob('*')):
        stat = p.lstat()
        result[str(p.relative_to(world))] = (stat.st_mode, stat.st_ino, stat.st_mtime_ns,
            os.readlink(p) if p.is_symlink() else hashlib.sha256(p.read_bytes()).hexdigest() if p.is_file() else None)
    return result


def compare(world, dest):
    expected = {}
    for path in (world / 'chunks').glob('*.bin'):
        root = parse(zlib.decompress(path.read_bytes(), wbits=47))
        key = tuple(int.from_bytes(value(root, x), 'big', signed=True) for x in ('xPos', 'zPos'))
        expected[key] = root
    actual = {key: root for key, root, _ in chunks(dest)}
    assert actual.keys() == expected.keys()
    for key, before in expected.items():
        after = copy.deepcopy(actual[key])
        assert value(after, 'DataVersion') == struct.pack('>i', 4788)
        for sy, old in sections(before).items():
            new = sections(after)[sy]
            bs = value(old, 'block_states')
            compact = True
            if bs:
                size = len(value(bs, 'palette')[1])
                bits = max(4, (size - 1).bit_length())
                words = len(value(bs, 'data', b'')) // 8
                compact = size == 1 or words == (4096 * bits + 63) // 64
            assert section_blocks(old, compact=compact) == section_blocks(new)
            # Packing is the only permitted structural difference in sections.
            bs = value(old, 'block_states')
            if bs and b'data' in bs:
                value(new, 'block_states')[b'data'] = copy.deepcopy(bs[b'data'])
        after[b'DataVersion'] = before[b'DataVersion']
        assert before == after, (key, 'unknown NBT/types/values changed')
    assert not (dest / 'chunks').exists()
    return len(expected)


class MigrationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='goatcore-s3c-cli-')
        self.base = Path(self.tmp.name)
        self.source = self.base / 'source'
        self.source.mkdir()
        (self.source / '.goatcore-storage.lock').touch()
        (self.source / '.goatcore-storage').write_text('bin\n')
        self.dest = self.base / 'converted'
        write_chunk(self.source, fixture())

    def tearDown(self):
        for p in self.base.rglob('*'):
            if p.is_dir() and not p.is_symlink():
                p.chmod(0o700)
        self.tmp.cleanup()

    def command(self, mode='convert', tool=None, destination=None):
        return [str(tool or TOOL), '--' + mode, '--source', str(self.source),
                '--destination', str(destination or self.dest), '--source-offline']

    def invoke(self, mode='convert', expected=0, **kw):
        before = snapshot(self.source)
        cmd = self.command(mode, tool=kw.pop('tool', None), destination=kw.pop('destination', None))
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30, **kw)
        self.assertNotRegex(result.stderr, r'AddressSanitizer|LeakSanitizer|ThreadSanitizer|runtime error:')
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        self.assertEqual(before, snapshot(self.source), 'source was modified')
        rows = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
        self.assertEqual(rows[-1]['event'], 'summary')
        EVIDENCE.append(dict(case=self._testMethodName, operation=mode, expected_exit=expected,
                             actual_exit=result.returncode, source_unchanged=True, summary=rows[-1]))
        return rows[-1], rows

    def test_analyze_convert_verify_and_opaque_files(self):
        player = self.source / 'players/data/test.dat'
        player.parent.mkdir(parents=True)
        player.write_bytes(b'opaque binary player data, copied without interpretation')
        write_chunk(self.source, fixture(-33, 32, 4785), gzip=True)
        write_chunk(self.source, fixture(32, -33))
        r, _ = self.invoke('analyze')
        self.assertEqual((r['accepted'], r['converted'], r['regions']), (3, 0, 3))
        self.assertFalse(self.dest.exists())
        r, _ = self.invoke()
        self.assertEqual((r['converted'], r['verified'], r['accepted']), (3, 3, 3))
        self.assertTrue(r['durable_publication'])
        self.assertEqual(compare(self.source, self.dest), 3)
        self.assertEqual(player.read_bytes(), (self.dest / player.relative_to(self.source)).read_bytes())
        self.invoke('verify')
        self.invoke(expected=1)

    def test_unknown_nbt_and_seven_supported_entities(self):
        root = fixture()
        names = [
            ('chest', 'chest', {'facing': 'north', 'type': 'single', 'waterlogged': 'false'}),
            ('furnace', 'furnace', {'facing': 'north', 'lit': 'false'}),
            ('sign', 'oak_sign', {'rotation': '0', 'waterlogged': 'false'}),
            ('banner', 'white_banner', {'rotation': '0'}),
            ('shulker_box', 'shulker_box', {'facing': 'up'}),
            ('barrel', 'barrel', {'facing': 'north', 'open': 'false'}),
            ('decorated_pot', 'decorated_pot', {'facing': 'north', 'cracked': 'false', 'waterlogged': 'false'})]
        bs = value(sections(root)[4], 'block_states')
        palette = value(bs, 'palette')[1]
        entities = value(root, 'block_entities')[1]
        indices = [i % 2 for i in range(4096)]
        for i, (entity, block, properties) in enumerate(names):
            indices[i] = len(palette)
            palette.append({b'Name': (8, ('minecraft:' + block).encode()),
                            b'Properties': (10, {k.encode(): (8, v.encode()) for k, v in properties.items()})})
            be = {b'id': (8, ('minecraft:' + entity).encode()), b'x': integer(i), b'y': integer(64), b'z': integer(0),
                  b'Opaque': (10, {b'int_array': (11, struct.pack('>iii', -1, 775, 4788))})}
            if entity in ('chest', 'furnace', 'shulker_box', 'barrel'):
                be[b'Items'] = (9, (10, [{b'Slot': (1, b'\0'), b'id': (8, b'minecraft:stone'), b'count': integer(5),
                                          b'components': (10, {b'minecraft:custom_data': (10, {b'v': (1, b'\xef')})})}]))
            entities.append(be)
        bs[b'data'] = (12, packed(indices, 4))
        write_chunk(self.source, root)
        self.invoke()
        self.assertEqual(compare(self.source, self.dest), 1)
        self.invoke('verify')

    def test_refuse_legacy_inventory_types_and_missing_entity(self):
        root = fixture()
        bs = value(sections(root)[4], 'block_states')
        value(bs, 'palette')[1][0] = {b'Name': (8, b'minecraft:chest'), b'Properties': (10, {
            b'facing': (8, b'north'), b'type': (8, b'single'), b'waterlogged': (8, b'false')})}
        write_chunk(self.source, root)
        self.invoke(expected=1)
        self.assertFalse(self.dest.exists())
        bs[b'data'] = (12, packed([1] * 4096, 4))
        root[b'block_entities'] = (9, (10, [{b'id': (8, b'minecraft:chest'), b'x': integer(0), b'y': integer(64), b'z': integer(0),
            b'Items': (9, (10, [{b'slot': (1, b'\0'), b'id': (8, b'minecraft:stone'), b'count': (1, b'\x05')}]))}]))
        write_chunk(self.source, root)
        self.invoke(expected=1)

    def test_refuse_malformed_versions_states_coordinates_and_compression(self):
        root = fixture()
        cases = []
        bad = copy.deepcopy(root); bad[b'DataVersion'] = integer(1); cases.append(encode(bad))
        bad = copy.deepcopy(root); bad[b'xPos'] = integer(1); cases.append(encode(bad))
        bad = copy.deepcopy(root); bad[b'Status'] = (8, b'minecraft:noise'); cases.append(encode(bad))
        bad = copy.deepcopy(root); value(value(sections(bad)[4], 'block_states'), 'palette')[1][0][b'Name'] = (8, b'minecraft:unknown'); cases.append(encode(bad))
        cases += [encode(root) + b'\0', encode(root)[:-1], encode(root)[:-1] + b'\x03\0\x04xPos\0\0\0\0\0']
        for i, raw in enumerate(cases):
            with self.subTest(i=i):
                (self.source / 'chunks/c_0_0.bin').write_bytes(zlib.compress(raw))
                r, _ = self.invoke(expected=1)
                self.assertFalse(r['complete'])
                self.assertFalse(self.dest.exists())
        for raw in (b'invalid', zlib.compress(encode(root)) + b'trailing'):
            (self.source / 'chunks/c_0_0.bin').write_bytes(raw)
            self.invoke(expected=1)

    def test_refuse_duplicates_sidecars_and_mixed_backend(self):
        root = fixture()
        write_chunk(self.source, root, name='c_00_0.bin')
        r, rows = self.invoke(expected=1)
        self.assertEqual(r['accepted'], 0)
        self.assertTrue(any('duplicate' in row.get('reason', '') for row in rows))
        (self.source / 'chunks/c_00_0.bin').unlink()
        for name in ('containers/test.mct', 'region/r.0.0.mca', 'dimensions/minecraft/overworld/region/r.0.0.mca', 'c.0.0.mcc', 'r.0.0.mca.s2-journal'):
            path = self.source / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(b'recoverable')
            self.invoke(expected=1); path.unlink()

    def test_partial_source_is_not_published_and_external_size_is_preflighted(self):
        bad = fixture(1, 0, 1)
        write_chunk(self.source, bad)
        r, _ = self.invoke(expected=1)
        self.assertEqual((r['accepted'], r['converted'], r['verified']), (1, 0, 0))
        self.assertFalse(self.dest.exists())
        (self.source / 'chunks/c_1_0.bin').unlink()
        oversized = fixture()
        oversized[b'LargeOpaque'] = (7, random.Random(7754788).randbytes(1100000))
        write_chunk(self.source, oversized)
        r, rows = self.invoke(expected=1)
        self.assertTrue(any('.mcc' in row.get('reason', '') for row in rows))
        self.assertEqual(r['converted'], 0)
        self.assertFalse(list(self.base.glob('.goatcore-migrate.tmp.*')))

    def test_refuse_symlinks_hardlinks_and_nonfresh_destination(self):
        outside = self.base / 'outside'; outside.write_bytes(b'personal canary')
        path = self.source / 'unknown'; path.symlink_to(outside)
        self.invoke(expected=1); path.unlink()
        os.link(outside, path)
        self.invoke(expected=1); path.unlink()
        self.dest.mkdir(); canary = self.dest / 'canary'; canary.write_bytes(b'intact')
        self.invoke(expected=1)
        self.assertEqual(canary.read_bytes(), b'intact')
        self.invoke(expected=1, destination=self.source)
        self.invoke(expected=1, destination=self.source / 'nested')
        alias = self.base / 'alias'; alias.symlink_to(self.source, target_is_directory=True)
        command = self.command(); command[3] = str(alias)
        p = subprocess.run(command, capture_output=True, timeout=10)
        self.assertEqual(p.returncode, 1)

    def test_busy_goatcore_and_vanilla_source_locks(self):
        with (self.source / '.goatcore-storage.lock').open('rb') as fd:
            fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
            self.invoke(expected=1)
        session = self.source / 'session.lock'; session.touch()
        # A separate owner is essential: snapshot() opens/closes source files,
        # which would release a POSIX lock owned by this Python process.
        holder = subprocess.Popen([sys.executable, '-c',
            'import fcntl,sys; f=open(sys.argv[1],"r+b"); fcntl.lockf(f,fcntl.LOCK_EX); '
            'print("ready",flush=True); sys.stdin.read(1)', str(session)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        try:
            self.assertEqual(holder.stdout.readline(), b'ready\n')
            self.invoke(expected=1)
        finally:
            holder.communicate(b'G', timeout=10)
        def competing_vanilla():
            p = subprocess.run([sys.executable, '-c',
                'import fcntl,sys; f=open(sys.argv[1],"r+b"); '
                'fcntl.lockf(f,fcntl.LOCK_EX|fcntl.LOCK_NB)', str(session)], capture_output=True, timeout=10)
            self.assertNotEqual(p.returncode, 0, 'source lock vanished during hashing')
        code, text, _ = self.gate('publish-before', competing_vanilla)
        self.assertEqual(code, 0, text)

    def test_source_readonly_and_destination_tampering(self):
        for p in self.source.rglob('*'):
            if p.is_file(): p.chmod(0o444)
            elif p.is_dir(): p.chmod(0o555)
        self.source.chmod(0o555)
        try:
            self.invoke()
            self.invoke('verify')
            (self.dest / 'extra').write_bytes(b'not in manifest')
            self.invoke('verify', expected=1)
        finally:
            self.source.chmod(0o700)

    def test_modified_manifest_or_region_is_not_verified(self):
        self.invoke()
        manifest = self.dest / 'goatcore-migration.json'
        original = manifest.read_bytes()
        manifest.write_bytes(original + b' ')
        self.invoke('verify', expected=1)
        manifest.write_bytes(original)
        region = next((self.dest / 'dimensions/minecraft/overworld/region').glob('*.mca'))
        image = bytearray(region.read_bytes())
        entry = int.from_bytes(image[:4], 'big');offset = (entry >> 8) * 4096
        image[offset:offset + 4] = b'\x7f\xff\xff\xff'
        region.write_bytes(image)
        self.invoke('verify', expected=1)

    def test_report_redirection_cannot_append_to_world_files(self):
        for fd in ('stdout', 'stderr'):
            log = self.source / ('forbidden-' + fd)
            with log.open('ab') as stream:
                kw = dict(stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                kw[fd] = stream
                p = subprocess.run(self.command(), timeout=10, **kw)
            self.assertEqual(p.returncode, 2)
            self.assertEqual(log.read_bytes(), b'')
        self.dest.mkdir(); canary = self.dest / 'canary';canary.write_bytes(b'intact')
        with canary.open('ab') as output:
            p = subprocess.run(self.command(), stdout=output, stderr=subprocess.PIPE, timeout=10)
        self.assertEqual(p.returncode, 2)
        self.assertEqual(canary.read_bytes(), b'intact')

    def gate(self, phase, change=None):
        ready_r, ready_w = os.pipe(); control_r, control_w = os.pipe()
        env = dict(os.environ, MC_MIGRATION_TEST_PHASE=phase,
                   MC_MIGRATION_TEST_READY=str(ready_w), MC_MIGRATION_TEST_RELEASE=str(control_r))
        p = subprocess.Popen(self.command(tool=FAULT), env=env, pass_fds=(ready_w, control_r),
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        os.close(ready_w); os.close(control_r)
        try:
            self.assertTrue(select.select([ready_r], [], [], 15)[0], phase)
            self.assertEqual(os.read(ready_r, 1), b'R', phase)
            if change:
                change(); os.write(control_w, b'G')
            else:
                p.kill()
            stdout, stderr = p.communicate(timeout=30)
            self.assertNotRegex(stderr.decode(), r'AddressSanitizer|LeakSanitizer|ThreadSanitizer|runtime error:')
            return p.returncode, stdout.decode(), stderr.decode()
        finally:
            if p.poll() is None:
                p.kill(); p.communicate(timeout=30)
            os.close(ready_r); os.close(control_w)

    def test_sigkill_matrix_preserves_source_and_relaunch(self):
        initial = snapshot(self.source)
        for phase in CRASH_PHASES:
            with self.subTest(phase=phase):
                self.dest = self.base / ('result-' + phase)
                code, _, _ = self.gate(phase)
                self.assertEqual(code, -signal.SIGKILL)
                self.assertEqual(initial, snapshot(self.source))
                if phase in ('publish-after', 'durable'):
                    self.assertEqual(compare(self.source, self.dest), 1)
                    self.invoke('verify')
                    self.invoke(expected=1)
                else:
                    self.assertFalse(self.dest.exists())
                    self.invoke()
                    self.invoke('verify')
                EVIDENCE.append(dict(case=self._testMethodName, phase=phase, signal='SIGKILL',
                    source_unchanged=True, coherent_after_publication=phase in ('publish-after','durable'),
                    fresh_relaunch_or_verification='passed'))

    def test_syscall_failures_uncertain_publish_and_retries(self):
        errors = [('read-before', 'pread-EIO'), ('prepare', 'mkdir-ENOSPC'),
                  ('region-write', 'write-ENOSPC'), ('region-write', 'fsync-EIO'),
                  ('files-sync-before', 'fsync-EIO'), ('publish-before', 'rename-EIO'),
                  ('publish-after', 'fsync-EIO'), ('publish-before', 'rename-after-EIO')]
        for i, (phase, error) in enumerate(errors):
            with self.subTest(error=(phase, error)):
                self.dest = self.base / ('error-' + str(i))
                env = dict(os.environ, MC_MIGRATION_TEST_PHASE=phase, MC_MIGRATION_TEST_ERROR=error)
                uncertain = phase == 'publish-after' or error == 'rename-after-EIO'
                r, _ = self.invoke(expected=3 if uncertain else 1, tool=FAULT, env=env)
                self.assertFalse(r['durable_publication'])
                if uncertain:
                    self.assertTrue(self.dest.exists())
                    self.assertEqual(compare(self.source, self.dest), 1)
                    self.invoke('verify')
                else:
                    self.assertFalse(self.dest.exists())
                    self.invoke()
        for error in ('write-short', 'write-EINTR', 'fsync-EINTR', 'pread-EINTR'):
            self.dest = self.base / ('recovered-' + error)
            env = dict(os.environ, MC_MIGRATION_TEST_PHASE='read-before' if 'pread' in error else 'region-write',
                       MC_MIGRATION_TEST_ERROR=error)
            self.invoke(tool=FAULT, env=env)
            self.assertEqual(compare(self.source, self.dest), 1)

    def test_source_change_and_destination_race_before_publish(self):
        def change_source():
            (self.source / 'new-data').write_bytes(b'concurrent writer')
        code, text, _ = self.gate('publish-before', change_source)
        self.assertEqual(code, 1, text)
        self.assertFalse(self.dest.exists())
        self.invoke()
        self.dest = self.base / 'race'
        def race():
            self.dest.mkdir(); (self.dest / 'canary').write_bytes(b'intact')
        code, text, _ = self.gate('publish-before', race)
        self.assertEqual(code, 1, text)
        self.assertEqual((self.dest / 'canary').read_bytes(), b'intact')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--tool', type=Path, required=True)
    parser.add_argument('--fault-tool', type=Path, required=True)
    parser.add_argument('--evidence', type=Path)
    args, rest = parser.parse_known_args()
    TOOL, FAULT = args.tool.resolve(), args.fault_tool.resolve()
    test = unittest.main(argv=[sys.argv[0], *rest], exit=False)
    if args.evidence:
        with args.evidence.open('x') as out:
            json.dump(dict(status='passed' if test.result.wasSuccessful() else 'failed',
                           tests=test.result.testsRun, crash_phases=CRASH_PHASES, events=EVIDENCE), out, indent=2)
    sys.exit(0 if test.result.wasSuccessful() else 1)
