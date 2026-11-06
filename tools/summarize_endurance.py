#!/usr/bin/env python3
"""Offline consolidation of collected endurance evidence; starts no server."""
import argparse
import hashlib
import json
from pathlib import Path


def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args):
    root=args.results;evidence={}
    def read(name):
        path=root/name;evidence[name]=digest(path);return json.loads(path.read_text())
    preflight=read('preflight/metrics/final.json')
    assert preflight['reason']=='manual-stop-file' and preflight['all_generators_stopped']
    assert preflight['restart_counts']=={'0':1}
    recovery=read('preflight/metrics/resilience-after-generator-restart.json');assert recovery['active']==5
    disconnected=read('preflight/metrics/resilience-before-generator-kill.json')
    assert disconnected['counts']['reconnections']==1 and disconnected['counts']['disconnects_or_errors']==1
    version=read('version/metrics/final.json')
    assert version['reason']=='published-origin-main-changed' and version['server_exit']['returncode']==0
    regression=read('regression/proof.json');assert regression['status']=='passed'
    assert (regression['attached_cases'],regression['hinged_cases'],regression['stairs_observations'])==(310,335,192)
    precommit=(root/'precommit-endurance.log').read_text();evidence['precommit-endurance.log']=digest(root/'precommit-endurance.log')
    assert precommit.count('[test] test_')==34 and 'status=0' in precommit
    candidate=read('candidate/metrics/manifest.json');final=read('candidate/metrics/final.json')
    assert final['all_generators_stopped'] and final['server_exit']['returncode']==0
    ramp=read('candidate/metrics/ramp.json') if (root/'candidate/metrics/ramp.json').exists() else []
    rows=[]
    for plateau in ramp:
        record=plateau['metrics'];count=plateau['bots']
        assert record['target']==record['active']==record['connected']==count
        assert record['counts'].get('failed_actions',0)==record['counts'].get('missing_actions',0)==0 and record['guard']['public_unchanged']
        assert not record['guard_warnings']
        process=record['processes'];server=record['server'];pid=server['pid'];tick=server['tick']
        server_proc=next(p for p in process if p['pid']==pid)
        generators=[p for p in process if p['pid']!=pid]
        rows.append(dict(bots=count,utc=record['utc'],elapsed_s=record['elapsed_s'],tps=tick['tps'],
            mspt={k:tick[k] for k in ('p50_ms','p95_ms','p99_ms','max_ms','over_50ms','busy_max_ms','busy_over_50ms')},
            tick_samples=tick['ticks_started'],server_cpu_percent=server_proc['cpu_percent'],
            generator_workers_cpu_percent=sum(p['cpu_percent'] or 0 for p in generators),server_memory=server_proc['status'],
            generator_sum_rss_kib=sum(int(p['status']['VmRSS'].split()[0]) for p in generators),
            counts=record['counts'],guard=record['guard'],cgroups=record['cgroups'],
            io=server_proc['io'],saves=server['saves'],latencies=record['latencies']))
    integrity=read('candidate/integrity.json');assert integrity['sqlite_quick_check']=='ok' and integrity['players_gzip_nbt_readable']
    restart=read('candidate/restart-proof.json');assert restart['status']=='passed' and len(restart['cases'])==5 and restart['admin_privileged_response']
    latest=read('candidate/metrics/latest.json')
    assert final['reason']=='test-tps-under-10' and latest['target']==150 and latest['connected']==150
    assert latest['counts'].get('failed_actions',0)==latest['counts'].get('missing_actions',0)==0
    assert [r['bots'] for r in rows]==[5,25,50,100]
    failed=read('failed-ramp/metrics/final.json');assert failed['reason']=='unconfirmed-action-error'
    cold=read('cold-lanes/metrics/cold-lanes-proof.json');assert cold['status']=='passed' and cold['snapshot']['counts']['cycles']==50 and not cold['snapshot']['errors']
    for name,checksum in candidate['script_hashes'].items():
        assert digest(Path(__file__).resolve().parents[1]/'tests/stress'/name)==checksum,name
    baseline=read('initial.json');after=read('after-validation.json')
    assert baseline['protected_sha256']==after['protected_sha256']
    assert after['public_pid']==147 and after['public_restarts']==0 and after['public_active']=='active'
    expected='d606558a906ed71b07298525b422601562f7d18b10163f78e74482672de59d5e'
    assert candidate['config']['binary_sha256']==regression['server_sha256']==expected
    output=dict(status='qualified observed plateaus; not human-player capacity',initial_head='7af270bb37c137ed521c169c5dfebc92a784197b',
        binary_sha256=expected,precommit_c_suites=34,regressions=regression,preflight=preflight,
        version_change_probe=dict(observed=version,method='deliberately mismatched expected SHA; real public API, no Git mutation'),
        candidate=dict(manifest=candidate,final=final,integrity=integrity,restart=restart),observed_plateaus=rows,
        interrupted_150=dict(connected=latest['connected'],active=latest['active'],inflight=latest['inflight_actions'],
            counts=latest['counts'],tick=latest['server']['tick'],guard=latest['guard'],processes=latest['processes'],
            saves=latest['server']['saves']),failed_fixture_attempt=failed,
        corrected_cold_lanes=dict(completed_cycles=50,counts=cold['snapshot']['counts'],errors=[]),
        validated_campaign_ceiling=100,not_qualified=[150,200,250],
        public_after=after,evidence_sha256=evidence,
        limits=['Short buffered tick windows, not plateau medians or constant 20 TPS',
            'Paced scripted Survival with artificial inventory/food grants, not human capacity',
            'No 1000-player qualification','No new C runtime changes; previous sanitizer results not rerun',
            'Component allocation counts and true physical RAID latency unavailable',
            'Published post-push campaign SHA and ongoing activity are in its runtime manifest, not this precommit evidence'])
    args.output.write_text(json.dumps(output,indent=2)+'\n')
    print(json.dumps(dict(observed_plateaus=[r['bots'] for r in rows],stop_reason=final['reason'],public_unchanged=True)))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--results',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    run(p.parse_args())
