#!/usr/bin/env python3
"""Instrument the integrated storage AND private SQLite. No Minecraft server.

Build outputs and all evidence stay in a new disposable results directory.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]
TARGETS = ('test_goatworld_storage', 'test_goatworld_storage_fault', 'test_goatworld',
           'test_save_pipeline', 'test_native_storage', 'test_stairs', 'test_nbt', 'test_hinged_blocks', 'test_attached_blocks',
           'test_anvil_roundtrip', 'test_pvp')


def run(output):
    output.mkdir(parents=True, exist_ok=False)
    subprocess.run(['make', 'storage-deps'], cwd=ROOT, check=True)
    results = {}
    for kind, sanitizer in (('asan', 'address,undefined'), ('tsan', 'thread')):
        out = output / kind
        out.mkdir()
        flags = f'-fsanitize={sanitizer} -fno-omit-frame-pointer -fno-pie -no-pie'
        library = out / 'libsqlite3.a'
        with (out / 'sqlite.build.log').open('w') as log:
            command = shlex.split('cc -std=c11 -O1 -g -pthread -DSQLITE_THREADSAFE=1 '
                                  '-DSQLITE_OMIT_LOAD_EXTENSION=1 ' + flags)
            subprocess.run(command + ['-c', '.local/storage-deps/sqlite3.c', '-o', out / 'sqlite3.o'],
                           cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run(['ar', 'rcs', library, out / 'sqlite3.o'], check=True)
        cflags = ('-std=c11 -O1 -g -Wall -Wextra -Iinclude -Isrc/generated -Isrc/world '
                  '-Isrc/util -I.local/storage-deps/include -D_POSIX_C_SOURCE=200809L -pthread ' + flags)
        ldflags = f'-lz -lm -pthread -lssl -lcrypto {library} -ldl -l:liblz4.so.1 -l:libzstd.so.1'
        hashes = {}
        for target in TARGETS:
            commands = subprocess.check_output(['make', '-n', '-B', target, 'CFLAGS=' + cflags,
                        'LDFLAGS=' + ldflags, 'SQLITE_LIB=' + str(library)], cwd=ROOT, text=True)
            command = shlex.split(next(line for line in commands.splitlines() if f'-o {target} ' in line))
            command[command.index('-o') + 1] = str(out / target)
            with (out / (target + '.build.log')).open('w') as log:
                subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
            hashes[target] = hashlib.sha256((out / target).read_bytes()).hexdigest()
        env = os.environ.copy()
        if kind == 'asan':
            env.update(ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',
                       UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1',
                       G1_SANITIZER_PRELOAD=subprocess.check_output(
                           ['gcc', '-print-file-name=libasan.so'], text=True).strip())
        else:
            env['TSAN_OPTIONS'] = 'halt_on_error=1'
        statuses = {}
        for target in TARGETS:
            if target.endswith('_fault'):
                continue
            with (out / (target + '.log')).open('w') as log:
                statuses[target] = subprocess.run([out / target], env=env, cwd=ROOT,
                    stdout=log, stderr=subprocess.STDOUT, timeout=300).returncode
        with (out / 'crash.log').open('w') as log:
            statuses['crash'] = subprocess.run(['python3', 'tests/test_goatworld_integrated.py',
                '--tool', out / 'test_goatworld_storage', '--fault-tool', out / 'test_goatworld_storage_fault',
                '--evidence', out / 'crash-evidence.json'], cwd=ROOT, env=env,
                stdout=log, stderr=subprocess.STDOUT, timeout=300).returncode
        proof = dict(statuses=statuses, sha256=hashes, flags=flags, sqlite_instrumented=True)
        (out / 'status.json').write_text(json.dumps(proof, indent=2) + '\n')
        results[kind] = proof
        print(kind, statuses, flush=True)
    (output / 'proof.json').write_text(json.dumps(results, indent=2) + '\n')
    return 0 if all(code == 0 for proof in results.values() for code in proof['statuses'].values()) else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--result-dir', type=Path, required=True)
    raise SystemExit(run(parser.parse_args().result_dir.resolve()))
