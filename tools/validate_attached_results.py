#!/usr/bin/env python3
"""Validate collected Phase 2B evidence without starting a Minecraft server.

Inputs are raw results from the isolated remote runners; outputs are small,
versionable observations and a manifest. This checks evidence, not gameplay.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return json.loads(path.read_text())


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate(args):
    raw = args.results
    binary = (raw / 'binaries.sha256').read_text().split()[0]
    evidence = {}

    def record(relative):
        path = raw / relative
        evidence[relative] = digest(path)
        return read(path)

    def suite(name, server=True):
        meta = record(name + '/metadata.json')
        assert meta['status'] == 'passed' and meta['verify_restarts'] >= 1, name
        if not name.startswith('nonregression/'):
            assert meta['verify_restarts'] == 2, name
        if server:
            assert meta['binary_sha256'] == binary, name
        report = record(name + '/report-edit.json')
        assert report['status'] == 'passed', name
        for suffix in ['verify'] + [f'verify-{i}' for i in range(2, meta['verify_restarts'] + 1)]:
            assert record(name + '/report-' + suffix + '.json')['status'] == 'passed'
        return report

    official = suite('vanilla-final-qualified', False)
    extra = suite('ladder-water-vanilla', False)
    for name in ('goatworld-final-qualified', 'bin', 'anvil'):
        actual = suite(name)
        assert actual['cases'] == official['cases'] and actual['saved'] == official['saved'], name
    actual = suite('ladder-water-goatworld')
    assert actual['cases'] == extra['cases'] and actual['saved'] == extra['saved']
    assert len(official['cases']) == 275 and len(extra['cases']) == 35
    hinged = suite('hinged')
    assert hinged['cases'] == read(ROOT / 'tests/fixtures/vanilla-26.1.1-hinged.json')['cases']
    stairs = suite('nonregression/stairs')
    vanilla_stairs = suite('nonregression/vanilla-stairs', False)
    assert stairs['cases'] == vanilla_stairs['cases'] and len(stairs['cases']) == 192
    regressions = record('nonregression/proof.json')
    assert regressions['status'] == 'passed' and regressions['nbt_types_values_exact']
    assert regressions['server_sha256'] == binary and len(regressions['block_entities']) == 7
    for name in ('placement', 'nonregression/placement', 'nonregression/pickup', 'nonregression/complex'):
        suite(name)
    gates = {name: record(name + '/proof.json') for name in ('async-loss', 'async-hinged')}
    assert all(proof['status'] == 'passed' for proof in gates.values())
    regeneration = record('regeneration.json')
    assert regeneration['status'] == 'passed' and len(regeneration['outputs']) == 6
    for output in regeneration['outputs']:
        assert digest(ROOT / output['path']) == output['sha256']
    services = record('services-after.json')
    assert services['protected_files_unchanged'] and not services['temporary_server_pids']
    evidence['precommit-final.log'] = digest(raw / 'precommit-final.log')
    precommit = (raw / 'precommit-final.log').read_text()
    assert precommit.count('[test] test_') == 32
    assert 'attached blocks: 547016 checks passed' in precommit

    runs = record('load-qualified/comparison.json')
    assert sorted((r['bots'], r['repetition']) for r in runs) == [
        (n, rep) for n in (1, 2, 10, 25, 50) for rep in (1, 2, 3)]
    performance = []
    for row in runs:
        bots = row['bots']
        assert row['sha256'] == binary and row['cycles_per_bot'] == 30
        assert row['missing'] == row['failures'] == 0
        assert row['received_actions'] == row['proposed_place'] + row['proposed_break'] == row['placed'] + row['broken'] == bots * 90
        assert row['observed_drop_entities'] == bots * 30
        assert row['restart_verification']['status'] == 'passed'
        assert row['restart_verification']['targets'] == row['durable_targets'] == bots * 2
        phase = row['phases']['active']
        selected = {k: phase[k] for k in (
            'duration_s', 'tps', 'p50_ms', 'p95_ms', 'p99_ms', 'max_ms',
            'over_50ms', 'busy_max_ms', 'busy_over_50ms', 'tick_and_fast_ms',
            'costs_s', 'fsync', 'process_cpu_percent', 'client_cpu_percent')}
        selected['storage'] = {k: phase['storage'].get(k) for k in (
            'support_neighbors', 'support_removed', 'handler_place', 'handler_break',
            'enqueue_place', 'enqueue_break', 'save_snapshot', 'sqlite_commit',
            'sqlite_transaction', 'save_backpressure')}
        folder = raw / 'load-qualified' / f"attached-{bots}-r{row['repetition']}"
        start, end = read(folder / 'actions.json')['phases']['active']
        sync_groups = {}
        with (folder / 'fsync.csv').open() as source:
            for entry in csv.reader(source):
                if len(entry) < 6 or not start <= int(entry[0]) < end:
                    continue
                name = 'players' if '/players/' in entry[5] else 'sqlite' if '/goatworld/' in entry[5] else 'other'
                group = sync_groups.setdefault(name, dict(calls=0, wall_s=0, errors=0))
                group['calls'] += 1
                group['wall_s'] += (int(entry[1]) - int(entry[0])) / 1e6
                group['errors'] += int(entry[4]) != 0
        selected['fsync_by_path'] = sync_groups
        performance.append(dict(bots=bots, repetition=row['repetition'],
            active=selected, received_actions=row['received_actions'],
            observed_drop_entities=row['observed_drop_entities'],
            total_including_shutdown_s=row['total_including_shutdown_s'],
            after_active_until_shutdown_s=row['after_active_until_shutdown_s'],
            restart_verification=row['restart_verification'],
            process=row['final_process_usage'], writes=row['world_write_trace'],
            shutdown_save=row['total_storage'].get('world_shutdown_save')))
    sanitizers = read(args.sanitizers / 'sanitizers-release/proof.json')
    wet = read(args.sanitizers / 'sanitizers-water/proof.json')
    sources = read(args.sanitizers / 'source-match.json')
    assert sources['status'] == 'passed'
    for name, checksum in sources['files'].items():
        assert digest(ROOT / name) == checksum, name
    for kind in ('asan', 'tsan'):
        assert all(code == 0 for code in sanitizers[kind]['statuses'].values())
        assert wet[kind]['returncode'] == 0 and wet[kind]['sqlite_instrumented']
    fixture = dict(version='26.1.1', protocol=775, data_version=4788,
        official_jar_sha256='3647564632a2f589be2a87bf544f628e5f67cb3780c86f7fa972ec36892aa081',
        blocks_report_sha256=digest(ROOT / 'data/26.1.1/reports/blocks.json'),
        description='Official server observations, two observers, two successive clean restarts. No GoatCore rule generates these expectations.',
        main=official, ladder_trap_water=extra)
    states = {c['state_id'] for r in (official, extra) for c in r['cases'] if 'state_id' in c}
    manifest = dict(status='passed', initial_head='d866b409209e7a34c84161098800b84e225ad010',
        server_sha256=binary, new_differential_cases=310, unique_observed_state_ids=len(states),
        official_block_states=63, official_shape_sets=126, official_aabbs=191,
        support_predicate_states=29873, c_suites=32, attached_checks=547016,
        hinged_cases=len(hinged['cases']), stairs_observations=192,
        block_entities=regressions, old_snapshot_gates=gates, regeneration=regeneration,
        sanitizers=sanitizers, final_water_sanitizers=wet,
        hardware=record('hardware.json'), performance=performance,
        services_after=services,
        matched_runtime_sources=sources,
        limitations=['Not a power-loss simulation', 'No complete light/fluid/climbing simulation',
            'Main hotbar/player saves remain synchronous', 'View/simulation distance 2 for load',
            'Process-requested writes are not physical RAID writes', 'No constant 20 TPS claim'],
        evidence_sha256=evidence)
    args.fixture.write_text(json.dumps(fixture, indent=2) + '\n')
    args.manifest.write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(dict(status='passed', cases=310, unique_states=len(states), load_runs=len(runs),
        confirmed_actions=sum(r['received_actions'] for r in runs),
        indirect_drops=sum(r['observed_drop_entities'] for r in runs))))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--results', type=Path, required=True)
    parser.add_argument('--sanitizers', type=Path, required=True)
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    validate(parser.parse_args())
