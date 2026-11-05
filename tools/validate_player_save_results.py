#!/usr/bin/env python3
"""Consolidate collected remote player-save evidence; starts no server."""
import argparse
import csv
import hashlib
import json
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args):
    root=args.results; evidence={}
    def read(name):
        path=root/name;evidence[name]=digest(path);return json.loads(path.read_text())
    binary=(root/'binary.sha256').read_text().split()[0]
    network=read('network-validated/proof.json')
    assert network['status']=='passed' and network['server_sha256']==binary and len(network['cases'])==6
    network_hotbar={}
    for count in (1,10):
        with (root/f'network-validated/hotbar-{count}/saves-1.csv').open() as source:
            packets=[r for r in csv.DictReader(source) if r['phase']=='player_hotbar']
        # The last packet per player follows reconnection. "changes" in the
        # raw proof counts the first sequence's packets, including an initial
        # no-op for actor 8 in the ten-player test; do not call that a mutation.
        mutations=sum(int(r['bytes']) for r in packets)-count
        assert len(packets)==109*count and mutations==100*count-(1 if count==10 else 0)
        network_hotbar[str(count)]=dict(initial_selection_packets=100*count,
            initial_true_mutations=mutations,explicit_repeat_packets=8*count,
            invalid_packets=2*count,post_reconnection_mutations=count)
    regression=read('regression-final/proof.json')
    assert regression['status']=='passed' and regression['server_sha256']==binary
    assert (regression['attached_cases'],regression['hinged_cases'],regression['stairs_observations'])==(310,335,192)
    sources=Path(__file__).resolve().parents[1]
    attached_reference=json.loads((sources/'tests/fixtures/vanilla-26.1.1-attached.json').read_text())
    hinged_reference=json.loads((sources/'tests/fixtures/vanilla-26.1.1-hinged.json').read_text())
    for label,reference in [('vanilla-attached',attached_reference['main']),
            ('attached',attached_reference['main']),('ladder-water',attached_reference['ladder_trap_water']),
            ('hinged',hinged_reference)]:
        actual=read(f'regression-final/{label}/report-edit.json')
        assert actual['status']=='passed' and actual['cases']==reference['cases'] and actual['saved']==reference['saved'],label
        for phase in ('verify','verify-2'):
            assert read(f'regression-final/{label}/report-{phase}.json')['status']=='passed'
    precommit=root/'precommit-final.log';text=precommit.read_text();evidence['precommit-final.log']=digest(precommit)
    assert text.count('[test] test_')==34 and '9 SIGKILL stages, 8 short/error/EINTR cases passed' in text
    assert 'attached blocks: 547016 checks passed' in text
    sanitizer_path=args.sanitizers/'proof.json';sanitizers=json.loads(sanitizer_path.read_text())
    for name in ('asan','tsan'):
        assert sanitizers[name]['sqlite_instrumented']
        assert len(sanitizers[name]['statuses'])==14 and all(rc==0 for rc in sanitizers[name]['statuses'].values())
    memory={str(n):read(f'memory-{n}/proof.json') for n in (4096,8192,16384,32768,65536,102400)}
    memory['pressure']=read('memory-pressure-bounded/proof.json')
    memory['refusal']=read('memory-refusal/proof.json')
    for name,data in memory.items():
        assert data['status']=='passed'
        if name!='refusal':assert data['server_sha256']==binary
        for event in ('oom','oom_kill'):
            before=dict(line.split() for line in data['before']['memory.events'].splitlines())
            after=dict(line.split() for line in data['after']['memory.events'].splitlines())
            assert before[event]==after[event]=='0'
    assert memory['pressure']['pressure'] and memory['pressure']['pressure_filler_mib']==512
    assert memory['refusal']['world_not_created']
    benchmark=read('bench/proof.json');assert benchmark['status']=='passed' and benchmark['same_work']
    results={}
    for mode in ('before','after'):
        raw=read(f'bench/{mode}/player-comparison.json')
        assert sorted((r['bots'],r['repetition']) for r in raw)==[(n,i) for n in (1,2,10,25,50) for i in (1,2,3)]
        rows=[]
        for r in raw:
            expected='3f09ae210cbd5382c93553c1fd692b968e206453c8e28b13d0c99deb0ca94bc1' if mode=='before' else binary
            assert r['sha256']==expected
            assert r['received_actions']==r['placed']+r['broken']==r['bots']*90
            assert r['missing']==r['failures']==0 and r['observed_drop_entities']==r['bots']*30
            assert r['restart_verification']['status']=='passed'
            assert len(r['players_verified'])==r['bots']
            folder=root/f"bench/{mode}/attached-{r['bots']}-r{r['repetition']}"
            sync=[]
            with (folder/'fsync.csv').open() as source:
                sync=[entry for entry in csv.reader(source) if '/players/' in entry[5]]
            all_sync=dict(calls=len(sync),wall_s=sum((int(e[1])-int(e[0]))/1e6 for e in sync),
                files=sum('/.player-' in e[5] for e in sync),directory_syncs=sum(e[5].endswith('/players/data') for e in sync),
                errors=sum(int(e[4])!=0 for e in sync))
            assert not all_sync['errors']
            active=r['phases']['active'];selected={key:active.get(key) for key in (
                'duration_s','tps','p50_ms','p95_ms','p99_ms','max_ms','over_50ms','busy_max_ms','busy_over_50ms',
                'tick_and_fast_ms','costs_s','task_queue_peak','client_cpu_percent','process_cpu_percent')}
            if mode=='after':
                assert r['active_player_fsync']['calls']==0
                assert active['storage']['player_hotbar']['calls']==r['bots']*60
                assert active['storage']['player_hotbar']['bytes']==r['bots']*59
                assert active['storage'].get('player_file_save',{}).get('calls',0)==0
            selected['storage']={k:active['storage'].get(k) for k in ('player_hotbar','player_immediate_request','player_file_save','save_snapshot','save_batch_submit','sqlite_commit','save_backpressure')}
            with (folder/'saves.csv').open() as source:
                batch_max=max((int(e['bytes']) for e in csv.DictReader(source) if e['phase']=='save_batch_submit'),default=0)
            assert batch_max<=64*1024*1024
            rows.append(dict(bots=r['bots'],repetition=r['repetition'],sha256=r['sha256'],active=selected,
                active_player_fsync=r['active_player_fsync'],all_player_data_fsync=all_sync,
                player_file_save=r['total_storage'].get('player_file_save'),
                confirmed_actions=r['received_actions'],observed_drop_entities=r['observed_drop_entities'],
                players_verified=r['players_verified'],latencies_ms=r['latencies_ms'],
                total_including_shutdown_s=r['total_including_shutdown_s'],after_active_until_shutdown_s=r['after_active_until_shutdown_s'],
                restart_verification=r['restart_verification'],world_writes=r['world_write_trace'],
                final_process_usage=r['final_process_usage'],rss_peak_observed_kb=r['rss_peak_observed_kb'],max_submitted_snapshot_bytes=batch_max))
        results[mode]=rows
    for before,after in zip(results['before'],results['after']):
        assert (before['bots'],before['repetition'],before['players_verified'])==(after['bots'],after['repetition'],after['players_verified'])
    services=read('services-after.json');assert services['protected_files_unchanged'] and not services['temporary_server_pids']
    source_match=read('source-match.json');assert source_match['status']=='passed'
    for name,checksum in source_match['files'].items():assert digest(sources/name)==checksum,name
    # The first periodic save is identified from the immutable trace, not an ACK.
    directory=root/'network-validated/hotbar-1'
    with (directory/'saves-1.csv').open() as source:trace=list(csv.DictReader(source))
    first=min(int(r['start_us']) for r in trace if r['phase']=='player_hotbar' and int(r['bytes']))
    periodic=min((r for r in trace if r['phase']=='player_file_save' and int(r['start_us'])>first),key=lambda r:int(r['start_us']))
    delay=(int(periodic['end_us'])-first)/1e6;assert 30<=delay<35
    manifest=dict(status='passed',initial_head='38696af6eb0634392324f88399b242983d2a50ca',server_sha256=binary,
        policy=dict(hotbar_flush_ms=30000,inventory_barriers='unchanged',async_player_worker=False,
            generation_bytes=32,minimum_target_mib=4096,high_target_mib=102400,reserve_mib=512,
            memory_guard='RLIMIT_AS, cgroup v2 visible limits; no preallocation or full cache accounting'),
        precommit_c_suites=34,player_crash_stages=9,player_io_error_cases=8,sanitizers=sanitizers,
        network=network,network_hotbar_counters=network_hotbar,
        periodic_confirmation_elapsed_s=delay,regressions=regression,memory_profiles=memory,
        benchmark_proof=benchmark,performance=results,services_after=services,runtime_source_match=source_match,
        hardware=read('hardware.json'),evidence_sha256=evidence,
        limits=['SIGKILL is not power loss','No global chunk/player/item transaction',
            'Periodic/forced player saves still synchronous','No constant 20 TPS or hundreds-of-players capacity claim',
            'Memory budget is configured capacity, not actual RSS or a fully adaptive cache engine',
            'No global cache purge or true hardware cold-read claim'])
    args.output.write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps(dict(status='passed',runs_per_version=15,actions_per_version=23760,periodic_elapsed_s=delay)))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--results',type=Path,required=True);p.add_argument('--sanitizers',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);run(p.parse_args())
