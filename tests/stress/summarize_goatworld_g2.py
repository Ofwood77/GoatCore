#!/usr/bin/env python3
"""Summarize completed G2 runs without opening or changing world files.

The only inputs are profile_tick's JSON and the optional stat-only WAL sampler.
Quantiles below are medians of per-run quantiles, not pooled distributions.
"""
import argparse
import json
from pathlib import Path
import statistics


def summarize(root, samples):
    rows = json.loads((root / 'comparison.json').read_text())
    assert len(rows) == 36, 'expected three repetitions, four loads, three modes'
    sizes = {}
    if samples:
        for line in samples.read_text().splitlines():
            sample = json.loads(line)
            target = sizes.setdefault(sample['run'], {})
            for name, info in sample['files'].items():
                target[name] = max(target.get(name, 0), info['bytes'])
    runs = []
    for row in rows:
        assert row['missing'] == row['failures'] == 0
        assert row['placed'] == row['broken'] == row['bots'] * 100
        assert row['acknowledged_place'] == row['proposed_place'] == row['placed']
        assert row['acknowledged_break'] == row['proposed_break'] == row['broken']
        assert row['restart_verification']['status'] == 'passed'
        assert row['final_process_usage']['write_trace_dropped'] == 0
        active = row['phases']['active']
        received_place = active['storage']['enqueue_place']['calls']
        received_break = active['storage']['enqueue_break']['calls']
        assert received_place == row['placed'] and received_break == row['broken']
        storage = row['total_storage']
        name = f"{row['label']}-{row['bots']}-r{row['repetition']}"
        runs.append(dict(
            name=name, mode=row['label'], bots=row['bots'], repetition=row['repetition'],
            sha256=row['sha256'], proposed_place=row['proposed_place'],
            proposed_break=row['proposed_break'], placed=row['placed'], broken=row['broken'],
            received_place=received_place, received_break=received_break,
            missing=0, final_sentinels=row['final_validation_placements'],
            durable_targets=row['durable_targets'], restart=row['restart_verification'],
            active_s=active['duration_s'], ticks=active['ticks_started'], tps=active['tps'],
            mspt_p50=active['p50_ms'], mspt_p95=active['p95_ms'], mspt_p99=active['p99_ms'],
            tick_max_ms=active['max_ms'], ticks_over_50=active['over_50ms'],
            busy_max_ms=active['busy_max_ms'], busy_over_50=active['busy_over_50ms'],
            tick_and_fast_ms=active['tick_and_fast_ms'],
            task_queue_max=active['task_queue_depth_max'], task_wait_max_ms=active['task_wait_max_ms'],
            action_latency_ms=row['latencies_ms'],
            pickup_observations=row['pickup_observations'],
            server_active_cpu_percent=active.get('process_cpu_percent'),
            python_active_cpu_percent=active.get('client_cpu_percent'),
            total_including_shutdown_s=row['total_including_shutdown_s'],
            after_active_until_shutdown_s=row['after_active_until_shutdown_s'],
            shutdown_s=row['phases']['shutdown']['duration_s'],
            durability_tail_s=row['phases']['durability_tail']['duration_s'],
            rss_peak_kb=row['rss_peak_observed_kb'], process=row['final_process_usage'],
            writes=row['world_write_trace'], storage=storage,
            fsync_active=active['fsync'], sampled_file_highwater_bytes=sizes.get(name, {})))
    groups = []
    for bots in (1, 10, 25, 50):
        for mode in ('bin', 'anvil', 'sqlite'):
            selected = [r for r in runs if r['mode'] == mode and r['bots'] == bots]
            assert len(selected) == 3
            med = lambda field: statistics.median(r[field] for r in selected)
            nested = lambda field, key: statistics.median(r[field][key] for r in selected)
            groups.append(dict(
                mode=mode, bots=bots, repetitions=3,
                tps_median=med('tps'), tps_min=min(r['tps'] for r in selected),
                tps_max=max(r['tps'] for r in selected),
                mspt_p50=med('mspt_p50'), mspt_p95=med('mspt_p95'), mspt_p99=med('mspt_p99'),
                max_tick_ms=max(r['tick_max_ms'] for r in selected),
                ticks_over_50_sum=sum(r['ticks_over_50'] for r in selected),
                max_busy_ms=max(r['busy_max_ms'] for r in selected),
                busy_over_50_sum=sum(r['busy_over_50'] for r in selected),
                action_p99_ms=nested('action_latency_ms', 'p99'),
                write_bytes_median=nested('writes', 'completed_bytes'),
                write_calls_median=nested('writes', 'calls'),
                fsync_calls_median=nested('process', 'fsync_calls'),
                process_write_bytes_median=nested('process', 'write_bytes'),
                cpu_s_median=statistics.median(r['process']['user_s'] + r['process']['system_s'] for r in selected),
                total_s_median=med('total_including_shutdown_s'), shutdown_s_median=med('shutdown_s'),
                durability_tail_s_median=med('durability_tail_s'), rss_peak_kb_max=max(r['rss_peak_kb'] for r in selected)))
    return dict(
        status='passed', runs=len(runs), groups=groups, per_run=runs,
        placed=sum(r['placed'] for r in runs), broken=sum(r['broken'] for r in runs),
        final_sentinels=sum(r['final_sentinels'] for r in runs), missing=0,
        caveats=['Medians of three independent per-run quantiles; not pooled quantiles.',
                 'busy metrics also include fast actions between scheduled ticks.',
                 'write/pwrite counts include deferred saves through shutdown, not restart verification.',
                 'Linux process write_bytes is not a physical RAID/controller measurement.',
                 'File highwater is sampled at 250 ms and may miss short peaks.',
                 'Placement/break work is equal; incidental pickups vary with timing and are recorded per observer, not summed as unique pickups.',
                 'No globally cold OS/device caches; startup integrity_check reads the database.'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--results', type=Path, required=True)
    parser.add_argument('--wal-samples', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.write_text(json.dumps(summarize(args.results, args.wal_samples), indent=2) + '\n')
