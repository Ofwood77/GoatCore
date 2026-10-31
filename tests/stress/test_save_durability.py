#!/usr/bin/env python3
"""S5 process-level Anvil durability checks, fresh disposable copies only.

Run on the test host, or under the explicit local maintenance exception.
Uses existing protocol-775 placement and independent typed-NBT oracles.
The preload gate is test-only; no fault hooks enter mc_server.
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
from types import SimpleNamespace

from native_storage_oracle import chunks, sections, section_blocks, preservation
from native_corpus import compare_entities
from test_block_placement import Placer, command, hold, place, observed, report_states
from test_block_persistence import query_block
from test_player_visibility import close, until
from protocol import string

ROOT = Path(__file__).resolve().parents[2]
POINT = (-10, 64, 26)
STAGES = ('write-before', 'partial-write', 'file-sync-before', 'file-sync-after',
          'rename-before', 'rename-after', 'dir-sync-before', 'dir-sync-after')


def disk_state(world):
    for xy, root, _ in chunks(world):
        if xy == (POINT[0] // 16, POINT[2] // 16):
            section = sections(root)[POINT[1] // 16]
            return section_blocks(section)[((POINT[1] % 16) * 16 + POINT[2] % 16) * 16 + POINT[0] % 16]
    raise AssertionError('target chunk missing; must never regenerate')


def exact_blocks(before, after, expected):
    target=(POINT[0]//16,POINT[2]//16)
    old=next(root for xy,root,_ in chunks(before) if xy==target)
    new=next(root for xy,root,_ in chunks(after) if xy==target)
    left,right=sections(old),sections(new)
    for sy in range(-4,20):
        want=section_blocks(left[sy]) if sy in left else ['minecraft:air']*4096
        actual=section_blocks(right[sy]) if sy in right else ['minecraft:air']*4096
        if sy==POINT[1]//16:
            want[((POINT[1]%16)*16+POINT[2]%16)*16+POINT[0]%16]=expected
        assert want==actual,(target,sy,'unrelated block changed')
    return dict(status='passed',checked_blocks=98304,only_allowed_change=list(POINT))


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    assert port not in (25565, 25587)
    return port


async def ready(process, port):
    deadline = asyncio.get_running_loop().time() + 30
    while True:
        assert process.poll() is None, ('server startup failed', process.returncode)
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=.1):
                return
        except OSError:
            assert asyncio.get_running_loop().time() < deadline, 'listen timeout'
            await asyncio.sleep(.05)


async def saved(bot):
    begin = len(bot.events[121])
    await bot.conn.send(7, string('save-all'))
    await until(lambda: any(b'loaded chunks durably saved' in p for _, p in bot.events[121][begin:]),
                'explicit save-all durable barrier', timeout=30)


async def stop(process, expected=0):
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
    code = await asyncio.to_thread(process.wait, 60)
    assert code == expected, ('server shutdown failed', code, expected)


def environment(runtime, port, mode):
    env = os.environ.copy()
    for key in list(env):
        if key.startswith('MC_S5_FAULT_') or key in ('LD_PRELOAD', 'MC_FSYNC_TRACE_PATH', 'MC_PROC_FINAL_PATH'):
            env.pop(key)
    env.update(MC_WORLD_PATH=str(runtime / 'world'), MC_BIND_PORT=str(port), MC_QA_MODE='1',
               MC_ADMIN_USERNAME='Ofwood_', MC_SETUP_FILE=str(ROOT / 'tests/stress/network/one.conf'))
    return env


async def connect(port):
    settings = SimpleNamespace(host='127.0.0.1', port=port, timeout=35, run_id='S5D',
                               shared_zone=False, mode='connect', qa_verify_movement=False)
    a, b = Placer(0, settings), Placer(1, settings)
    a.name = 'Ofwood_'
    for bot in (a, b):
        await bot.connect()
        await bot.conn.send(44)
    await command(a, 'gamemode creative')
    await command(a, 'tp -11.5 64 26.5')
    await command(a, f'tp {b.name} -13.5 64 26.5')
    for bot in (a, b):
        await asyncio.wait_for(bot.chunk_ready_events[(-1, 1)].wait(), 30)
    return a, b


async def restart(args, runtime, mode, expected, folder):
    port = free_port()
    with (folder / 'restart.log').open('w') as log:
        process = subprocess.Popen([str(args.server_binary)], cwd=runtime,
                                   env=environment(runtime, port, mode), stdout=log, stderr=subprocess.STDOUT)
        bots = []
        try:
            await ready(process, port)
            bots = list(await connect(port))
            state = report_states(ROOT / 'data/26.1.1/reports/blocks.json')[expected]
            assert await query_block(bots[0], POINT) == state
        finally:
            await asyncio.gather(*(close(bot) for bot in bots if bot.conn), return_exceptions=True)
            if process.poll() is None:
                await stop(process)
    assert process.returncode == 0


async def one(args, mode, action, stage, error=None):
    name = '-'.join(s for s in (mode, action, stage, error) if s)
    folder = args.result_dir / name
    folder.mkdir()
    runtime = folder / 'runtime'
    runtime.mkdir()
    world = runtime / 'world'
    shutil.copytree(args.world_template, world)
    (world / '.goatcore-s5-disposable').write_text('S5 fresh isolated copy\n')
    (runtime / 'assets').symlink_to(ROOT / 'assets')
    (runtime / 'server.config').write_text('online-mode=false\noffline-skins=false\nview-distance=2\n'
        'simulation-distance=2\nchunk-storage=anvil-atomic-experimental\nanvil-save-mode=' + mode + '\n')
    assert disk_state(world) == 'minecraft:air', 'fixture target must start as air'
    port = free_port()
    r, w = os.pipe()
    control_r, control_w = os.pipe()
    env = environment(runtime, port, mode)
    env.update(MC_SAVE_TRACE_PATH=str(folder / 'saves.csv'))
    if stage:
        env.update(LD_PRELOAD=str(args.fault_probe), MC_S5_FAULT_ROOT=str(world),
                   MC_S5_FAULT_STAGE=stage, MC_S5_FAULT_READY=str(w), MC_S5_FAULT_RELEASE=str(control_r))
        if error:
            env['MC_S5_FAULT_ERROR'] = error
    bots = []
    process = None
    expected = 'minecraft:stone'
    evidence = dict(mode=mode, action=action, stage=stage, error=error, port=port)
    with (folder / 'server.log').open('w') as log:
        try:
            process = subprocess.Popen([str(args.server_binary)], cwd=runtime, env=env,
                pass_fds=(w, control_r), stdout=log, stderr=subprocess.STDOUT)
            os.close(w); w = -1
            os.close(control_r); control_r = -1
            await ready(process, port)
            bots = list(await connect(port))
            await hold(bots[0], 1)
            await saved(bots[0])
            before = disk_state(world)
            assert before == 'minecraft:air'
            if stage:
                (world / '.s5-fault-armed').write_text('armed after setup save-all\n')
            for bot in bots:
                bot.clear_block_events(POINT)
            await place(bots[0], (POINT[0], POINT[1] - 1, POINT[2]), 1, (.5, 1., .5))
            await observed(bots[0], POINT, 1)
            evidence['actor_state_and_sequence_ack'] = True
            if stage:
                assert await asyncio.wait_for(asyncio.to_thread(os.read, r, 1), 20) == b'R', 'fault gate absent'
                evidence['gate_entered'] = True
                if mode == 'async' and action in ('term-newer', 'error', 'error-permanent'):
                    await command(bots[0], 'setblock -10 64 26 minecraft:dirt')
                    assert await query_block(bots[0], POINT) == report_states(ROOT/'data/26.1.1/reports/blocks.json')['minecraft:dirt']
                    expected = 'minecraft:dirt'
                    evidence['newer_live_mutation_confirmed'] = True
                if action == 'kill':
                    process.kill()
                    assert await asyncio.to_thread(process.wait, 30) == -signal.SIGKILL
                    expected = 'minecraft:stone' if stage in ('rename-after', 'dir-sync-before', 'dir-sync-after') else 'minecraft:air'
                elif action in ('term', 'term-newer'):
                    process.send_signal(signal.SIGTERM)
                    await asyncio.sleep(.15)
                    assert process.poll() is None, 'shutdown abandoned blocked accepted save'
                    os.write(control_w, b'G')
                    await stop(process)
                elif action in ('error', 'error-permanent'):
                    os.write(control_w, b'G')
                    await until(lambda: 'dirty retained' in (folder/'server.log').read_text(), 'latched save error')
                    if action == 'error':
                        await saved(bots[0])  # controlled retry must confirm newest generation
                        await stop(process)
                    else:
                        await stop(process, expected=2)
                        expected = 'minecraft:dirt' if stage == 'dir-sync-before' else 'minecraft:air'
                        evidence['durable_success_reported'] = False
                        assert 'world shutdown persistence FAILED' in (folder/'server.log').read_text()
                else:
                    raise AssertionError(action)
            else:
                await asyncio.gather(*(observed(bot, POINT, 1) for bot in bots[1:]))
                if action in ('clean', 'durable-kill'):
                    await saved(bots[0])
                if action == 'durable-kill':
                    evidence['save_all_acknowledged_before_kill'] = True
                    process.kill()
                    assert await asyncio.to_thread(process.wait, 30) == -signal.SIGKILL
                else:
                    await stop(process)
            evidence['exit'] = process.returncode
        finally:
            # A failed assertion must never leave a blocked test writer behind.
            if control_w >= 0:
                os.close(control_w); control_w = -1
            if process and process.poll() is None:
                process.kill()
                await asyncio.to_thread(process.wait, 30)
            await asyncio.gather(*(close(bot) for bot in bots if bot.conn), return_exceptions=True)
            for fd in (r, w, control_r):
                if fd >= 0:
                    os.close(fd)
    assert disk_state(world) == expected, (name, disk_state(world), expected)
    evidence['expected'] = expected
    evidence['blocks_before_restart'] = exact_blocks(args.world_template,world,expected)
    evidence['entities_before_restart'] = compare_entities(args.world_template, world, [(-1, 1)])
    evidence['preservation'] = preservation(args.world_template, world)
    await restart(args, runtime, mode, expected, folder)
    assert disk_state(world) == expected
    evidence['blocks_after_restart'] = exact_blocks(args.world_template,world,expected)
    evidence['entities_after_restart'] = compare_entities(args.world_template, world, [(-1, 1)])
    evidence['preservation_after_restart'] = preservation(args.world_template, world)
    assert not (world/'chunks').exists()
    evidence['status'] = 'passed'
    (folder / 'proof.json').write_text(json.dumps(evidence, indent=2))
    print(json.dumps(evidence), flush=True)
    return evidence


async def main(args):
    args.result_dir = args.result_dir.resolve()
    args.result_dir.mkdir(parents=True, exist_ok=False)
    for key in ('server_binary', 'fault_probe', 'world_template'):
        setattr(args, key, getattr(args, key).resolve())
    rows = []
    async def record(mode, action, stage, error=None):
        rows.append(await one(args, mode, action, stage, error))
        (args.result_dir/'report.json').write_text(json.dumps(rows, indent=2))
    for mode in args.modes:
        for action in ('clean', 'pending-stop', 'durable-kill'):
            if action in args.actions:
                await record(mode, action, None)
        for stage in args.stages:
            if 'kill' in args.actions:
                await record(mode, 'kill', stage)
        for stage in ('partial-write', 'file-sync-after', 'dir-sync-before'):
            if 'term' in args.actions and stage in args.stages:
                await record(mode, 'term-newer' if mode == 'async' else 'term', stage)
        if mode == 'async' and 'error' in args.actions:
            for stage, error in (('write-before', 'ENOSPC'), ('file-sync-before', 'EIO'), ('dir-sync-before', 'EIO')):
                if stage in args.stages:
                    await record(mode, 'error', stage, error)
        if mode == 'async' and 'error-permanent' in args.actions:
            for stage in ('file-sync-before', 'dir-sync-before'):
                if stage in args.stages:
                    await record(mode, 'error-permanent', stage, 'EIO-permanent')
    (args.result_dir/'manifest.json').write_text(json.dumps(dict(
        binary_sha256=hashlib.sha256(args.server_binary.read_bytes()).hexdigest(),
        probe_sha256=hashlib.sha256(args.fault_probe.read_bytes()).hexdigest(),
        passes=len(rows),method='process SIGKILL/SIGTERM and deterministic gate/error; not a physical power failure'),indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('server-binary', 'fault-probe', 'world-template', 'result-dir'):
        parser.add_argument('--'+key, type=Path, required=True)
    parser.add_argument('--modes', nargs='+', choices=('immediate','batched','async'), default=['immediate','batched','async'])
    parser.add_argument('--actions', nargs='+', choices=('clean','pending-stop','durable-kill','kill','term','error','error-permanent'),
                        default=['clean','pending-stop','durable-kill','kill','term','error','error-permanent'])
    parser.add_argument('--stages', nargs='+', choices=STAGES, default=list(STAGES))
    asyncio.run(main(parser.parse_args()))
