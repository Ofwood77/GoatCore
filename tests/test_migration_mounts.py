#!/usr/bin/env python3
"""Bind-mount alias protection in an isolated, unprivileged mount namespace."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from test_world_migration import fixture, write_chunk, snapshot


def worker(tool):
    with tempfile.TemporaryDirectory(prefix='goatcore-s3c-mount-') as temporary:
        base = Path(temporary); source = base/'source'; source.mkdir()
        write_chunk(source, fixture())
        (source/'.goatcore-storage').write_text('bin\n')
        (source/'.goatcore-storage.lock').touch()
        (source/'nested').mkdir(); (source/'nested/preserved').write_bytes(b'unchanged')
        mounts = []
        def bind(original, name):
            alias = base/name; alias.mkdir()
            subprocess.run(['mount', '--bind', original, alias], check=True)
            mounts.append(alias); return alias
        try:
            aliases = [bind(source, 'whole-alias'), bind(source/'nested', 'subtree-alias')]
            for alias in aliases:
                for mode in ('analyze', 'convert'):
                    before = snapshot(source)
                    result = subprocess.run([tool, '--'+mode, '--source', source,
                        '--destination', alias/'new', '--source-offline'], capture_output=True, text=True)
                    assert result.returncode == 1, result.stderr
                    assert 'aliases source directory' in result.stdout
                    assert snapshot(source) == before and not (alias/'new').exists()
            for alias in aliases:
                path = alias/'nested/preserved' if alias == aliases[0] else alias/'preserved'
                before = snapshot(source)
                with path.open('r+b') as report:
                    result = subprocess.run([tool, '--analyze', '--source', source,
                        '--destination', base/'new', '--source-offline'], stdout=report, stderr=subprocess.PIPE)
                assert result.returncode == 2 and snapshot(source) == before
            destination = base/'valid'
            subprocess.run([tool, '--convert', '--source', source, '--destination', destination,
                            '--source-offline'], stdout=subprocess.DEVNULL, check=True)
            alias = bind(destination, 'destination-alias')
            region = next(alias.rglob('*.mca')); before = snapshot(destination)
            with region.open('r+b') as report:
                result = subprocess.run([tool, '--verify', '--source', source,
                    '--destination', destination, '--source-offline'], stdout=report, stderr=subprocess.PIPE)
            assert result.returncode == 2 and snapshot(destination) == before
        finally:
            for alias in reversed(mounts):
                subprocess.run(['umount', alias], check=True)
    print(json.dumps(dict(status='passed', destination_alias_refusals=4,
                         source_report_alias_refusals=2, destination_report_alias_refusals=1,
                         source_unchanged=True)))


class MountAliases(unittest.TestCase):
    def test_bind_mounts_cannot_write_source_or_reports(self):
        if not all(shutil.which(name) for name in ('unshare', 'mount', 'umount')):
            self.skipTest('mount namespace tools unavailable')
        capability = subprocess.run(['unshare', '-Urnm', 'true'], capture_output=True)
        if capability.returncode:
            self.skipTest('unprivileged user/mount namespace unavailable')
        subprocess.run(['unshare', '-Urnm', sys.executable, str(Path(__file__).resolve()),
                        '--tool', TOOL, '--worker'], check=True, timeout=60)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tool', required=True); parser.add_argument('--worker', action='store_true')
    args = parser.parse_args(); TOOL = str(Path(args.tool).resolve())
    if args.worker: worker(TOOL)
    else: unittest.main(argv=[sys.argv[0]])
