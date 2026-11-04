#!/usr/bin/env python3
"""S3C end-to-end on disposable copies, local only under maintenance exception.

No source writes, main-service ports, deployment or automatic world adoption.
The optional official metadata template is a TEST composition, not migration.
"""
import argparse
import asyncio
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import time
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests'))
from test_world_migration import compare, snapshot
from native_storage_oracle import same_blocks, preservation
from native_corpus import compare_entities
from test_block_placement import Placer, command, observed, report_states
from test_player_visibility import close
from test_block_persistence import query_block
from test_save_durability import ready, saved

POINT = (-8, 200, 24)


def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0)); n = s.getsockname()[1]
    assert n not in (25565, 25587)
    return n


class TimedPlacer(Placer):
    first_chunk = None
    async def play_packet(self, packet_id, payload):
        if packet_id == 45 and len(payload) >= 8 and self.first_chunk is None:
            self.first_chunk = time.monotonic()
        await super().play_packet(packet_id, payload)


async def marker_cycle(args, runtime, edit, index):
    number = port(); start = time.monotonic()
    env = dict(os.environ, MC_WORLD_PATH=str(runtime / 'world'), MC_BIND_PORT=str(number),
               MC_SETUP_FILE=str(ROOT / 'tests/stress/network/one.conf'), MC_QA_MODE='1', MC_ADMIN_USERNAME='Ofwood_',
               MC_SAVE_TRACE_PATH=str(runtime / f'marker-load-save-{index}.csv'))
    for key in ('LD_PRELOAD', 'MC_IO_TRACE_PATH', 'MC_FSYNC_TRACE_PATH', 'MC_PERF_TRACE_PATH'):
        env.pop(key, None)
    bots = []
    with (runtime / f'marker-server-{index}.log').open('x') as log:
        process = subprocess.Popen([str(args.server)], cwd=runtime, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            await ready(process, number); listen = time.monotonic()
            opts = SimpleNamespace(host='127.0.0.1', port=number, timeout=35, run_id='S3C', shared_zone=False,
                                   mode='connect', qa_verify_movement=False)
            bots = [TimedPlacer(0, opts), TimedPlacer(1, opts)]; bots[0].name = 'Ofwood_'
            for bot in bots:
                await bot.connect(); await bot.conn.send(44)
            await command(bots[0], 'gamemode creative')
            for bot, x in zip(bots, (-11.5, -13.5)):
                await command(bots[0], f'tp {bot.name} {x} 64 26.5')
                await asyncio.wait_for(bot.chunk_ready_events[(-1, 1)].wait(), 30)
            state = report_states(ROOT / 'data/26.1.1/reports/blocks.json')
            expected = state['minecraft:air' if edit else 'minecraft:stone']
            assert await query_block(bots[0], POINT) == expected
            if edit:
                for bot in bots: bot.clear_block_events(POINT)
                await command(bots[0], 'setblock -8 200 24 minecraft:stone')
                await asyncio.gather(*(observed(bot, POINT, state['minecraft:stone']) for bot in bots))
            assert await query_block(bots[0], POINT) == state['minecraft:stone']
            barrier = time.monotonic(); await saved(bots[0]); barrier = time.monotonic() - barrier
            result = dict(index=index, edit=edit, listen_s=listen-start,
                          first_chunk_from_launch_s=bots[0].first_chunk-start,
                          first_chunk_after_listen_s=bots[0].first_chunk-listen,
                          durable_barrier_s=barrier, observers=2)
        finally:
            await asyncio.gather(*(close(bot) for bot in bots if bot.conn), return_exceptions=True)
            shutdown = time.monotonic()
            if process.poll() is None: process.send_signal(signal.SIGTERM)
            try: code = await asyncio.to_thread(process.wait, 60)
            finally:
                if process.poll() is None:
                    process.kill(); await asyncio.to_thread(process.wait, 10)
            assert code == 0, ('marker server shutdown failed', code)
    result['shutdown_s'] = time.monotonic()-shutdown
    return result


async def live_refusal(args, runtime):
    runtime.mkdir(); shutil.copytree(args.source, runtime / 'world')
    (runtime / 'assets').symlink_to(ROOT / 'assets')
    (runtime / 'server.config').write_text('online-mode=false\noffline-skins=false\nview-distance=2\nchunk-storage=bin\n')
    env = dict(os.environ, MC_WORLD_PATH=str(runtime / 'world'), MC_BIND_PORT=str(port()),
               MC_SETUP_FILE=str(ROOT / 'tests/stress/network/one.conf'))
    with (runtime / 'server.log').open('x') as log:
        process = subprocess.Popen([str(args.server)], cwd=runtime, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            await ready(process, int(env['MC_BIND_PORT']))
            done = await asyncio.to_thread(subprocess.run, [str(args.tool), '--analyze', '--source', str(runtime / 'world'),
                '--destination', str(runtime / 'forbidden'), '--source-offline'], capture_output=True, text=True, timeout=30)
            assert done.returncode == 1
            result = json.loads(done.stdout.splitlines()[-1])
            assert 'source in use' in result['reason'] and not (runtime / 'forbidden').exists()
            (runtime / 'refusal.json').write_text(json.dumps(result, indent=2))
        finally:
            if process.poll() is None: process.send_signal(signal.SIGTERM)
            try: code = await asyncio.to_thread(process.wait, 60)
            finally:
                if process.poll() is None:
                    process.kill(); await asyncio.to_thread(process.wait, 10)
            assert code == 0


def run(args):
    for name in ('source', 'snapshot', 'tool', 'server', 'vanilla_root', 'metadata_template'):
        value = getattr(args, name)
        if value is not None: setattr(args, name, value.resolve())
    args.result_dir = args.result_dir.resolve(); args.result_dir.mkdir(parents=True, exist_ok=False)
    result = args.result_dir; original = snapshot(args.source)
    count = len(list((args.source / 'chunks').glob('*.bin')))
    converted = result / 'converted'
    asyncio.run(live_refusal(args, result / 'live-source-refusal'))
    def execute(name, cmd, timeout=900):
        with (result / (name + '.log')).open('x') as log:
            subprocess.run(list(map(str, cmd)), cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=timeout)
    for mode in ('analyze', 'convert', 'verify'):
        execute(mode, [args.tool, '--'+mode, '--source', args.source, '--destination', converted, '--source-offline'])
        summary = json.loads((result / (mode+'.log')).read_text().splitlines()[-1])
        assert summary['accepted'] == count and summary['complete']
        if mode == 'convert': assert summary['converted'] == summary['verified'] == count and summary['durable_publication']
    assert compare(args.source, converted) == count
    runner = [sys.executable, ROOT / 'tests/stress/run_block_checks.py']
    options = ['--suite', args.suite, '--view-distance', '2']
    if args.suite == 'placement': options += ['--blocks-only']
    if args.boundaries: options += ['--region-boundaries']
    gc = runner + ['--kind', 'goatcore', '--server-binary', args.server,
                   '--chunk-storage', 'anvil-atomic-experimental', '--anvil-save-mode', 'async', *options]
    execute('gc-read', gc + ['--port', port(), '--verify-only', '--world-template', converted,
                            '--snapshot-template', args.snapshot, '--verify-restarts', '2', '--result-dir', result/'gc-read'])
    read_world = result / 'gc-read/runtime/world'
    before_blocks = same_blocks(converted, read_world)
    (result / 'gc-read-all-blocks.json').write_text(json.dumps(before_blocks, indent=2))
    if args.skip_full_edit:
        edit_world = read_world
    else:
        execute('gc-edit', gc + ['--port', port(), '--world-template', read_world,
                                '--verify-restarts', '3', '--result-dir', result/'gc-edit'])
        edit_world = result / 'gc-edit/runtime/world'
    marker = result / 'marker'; marker.mkdir()
    shutil.copytree(edit_world, marker / 'world'); (marker / 'assets').symlink_to(ROOT / 'assets')
    (marker / 'server.config').write_text('online-mode=false\noffline-skins=false\nview-distance=2\n'
        'simulation-distance=2\nchunk-storage=anvil-atomic-experimental\nanvil-save-mode=async\n')
    cycles = [asyncio.run(marker_cycle(args, marker, edit, i)) for i, edit in enumerate((True, False, False))]
    (result / 'marker-cycles.json').write_text(json.dumps(cycles, indent=2))
    final_world = marker / 'world'
    if args.suite == 'complex':
        (result / 'entities-preserved.json').write_text(json.dumps(compare_entities(converted, final_world, [(-1, 1)]), indent=2))
        (result / 'opaque-preserved.json').write_text(json.dumps(preservation(converted, final_world), indent=2))
    # Preserve pristine converted output. Only a further TEST copy receives
    # metadata absent from historical GoatCore; the converter never fabricates it.
    vanilla_input = result / 'vanilla-input'; shutil.copytree(final_world, vanilla_input)
    added = []
    if args.metadata_template:
        for name in ('level.dat', 'level.dat_old', 'data', 'datapacks'):
            target = vanilla_input / name; source = args.metadata_template / name
            if target.exists() or not source.exists(): continue
            if source.is_dir(): shutil.copytree(source, target)
            else: shutil.copy2(source, target)
            added.append(name)
    assert (vanilla_input / 'level.dat').exists()
    (result / 'vanilla-composition.json').write_text(json.dumps(dict(
        kind='disposable validation composition, not converter output or full-world compatibility',
        added_global_metadata=added), indent=2))
    marker_snapshot = result / 'marker-snapshot.json'
    marker_snapshot.write_text(json.dumps(dict(point=list(POINT), block='minecraft:stone')))
    execute('vanilla', runner + ['--kind', 'vanilla', '--java', args.java.resolve(), '--vanilla-root', args.vanilla_root,
        '--suite', 'complex', '--port', port(), '--shutdown-timeout', str(args.vanilla_shutdown_timeout),
        '--verify-only', '--world-template', vanilla_input,
        '--snapshot-template', marker_snapshot, '--verify-restarts', '2', '--result-dir', result/'vanilla'])
    official = result / 'vanilla/runtime/world'
    blocks = same_blocks(final_world, official)
    (result / 'vanilla-all-blocks.json').write_text(json.dumps(blocks, indent=2))
    if args.suite == 'complex':
        # Official registered BE fields must persist; synthetic root opaque
        # fields are checked before Vanilla's own normalizations above.
        (result / 'entities-vanilla.json').write_text(json.dumps(compare_entities(converted, official, [(-1, 1)]), indent=2))
    assert snapshot(args.source) == original
    # Pristine destination must still verify after all tests ran on copies.
    execute('verify-final', [args.tool, '--verify', '--source', args.source, '--destination', converted, '--source-offline'])
    proof = dict(status='passed', source_chunks=count, converted=count, verified=count,
                 full_blocks_per_chunk=98304, source_unchanged=True, marker=POINT,
                 marker_cycles=cycles, vanilla_compared_chunks=blocks['chunks'],
                 binary_sha256=hashlib.sha256(args.server.read_bytes()).hexdigest(),
                 tool_sha256=hashlib.sha256(args.tool.read_bytes()).hexdigest())
    (result / 'proof.json').write_text(json.dumps(proof, indent=2)); print(json.dumps(proof))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    for key in ('source', 'snapshot', 'tool', 'server', 'vanilla-root', 'result-dir'):
        p.add_argument('--'+key, type=Path, required=True)
    p.add_argument('--metadata-template', type=Path)
    p.add_argument('--java', type=Path, default=Path('/usr/bin/java'), help='official reference JVM executable')
    p.add_argument('--vanilla-shutdown-timeout', type=int, default=180, help='bounded Vanilla save/stop wait on HDD, seconds')
    p.add_argument('--suite', choices=('stairs', 'placement', 'complex'), required=True)
    p.add_argument('--boundaries', action='store_true')
    p.add_argument('--skip-full-edit', action='store_true', help='only marker mutation; full suite covered separately')
    run(p.parse_args())
