#!/usr/bin/env python3
"""Same unchanged Phase 2B workload before/after; includes final player files.
Run on the remote host, with no other temporary server/benchmark running.
"""
import argparse
import csv
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import uuid
from native_storage_oracle import parse, integer
from test_player_visibility import offline_uuid

ROOT = Path(__file__).resolve().parents[2]


def canonical(obj):
    if isinstance(obj, bytes): return ['bytes', obj.hex()]
    if isinstance(obj, dict): return sorted((key.hex(), canonical(value)) for key,value in obj.items())
    if isinstance(obj, (list,tuple)): return [canonical(value) for value in obj]
    return obj


def collect(root):
    rows = json.loads((root/'comparison.json').read_text())
    for row in rows:
        directory = root/f"attached-{row['bots']}-r{row['repetition']}"
        active = json.loads((directory/'actions.json').read_text())['phases']['active']
        sync = dict(calls=0,wall_s=0,cpu_s=0,errors=0)
        with (directory/'fsync.csv').open() as source:
            for entry in csv.reader(source):
                if '/players/' not in entry[5] or not active[0] <= int(entry[0]) < active[1]: continue
                sync['calls'] += 1; sync['wall_s'] += (int(entry[1])-int(entry[0]))/1e6
                sync['cpu_s'] += int(entry[2])/1e6; sync['errors'] += int(entry[4]) != 0
        row['active_player_fsync'] = sync
        row['players_verified'] = []
        for index in range(row['bots']):
            name = f'StressATPR{index:05d}'
            path = directory/'runtime/world/players/data'/(str(uuid.UUID(bytes=offline_uuid(name)))+'.dat')
            player = parse(gzip.decompress(path.read_bytes()))
            assert integer(player,'SelectedItemSlot') == 1, (name,'final selected slot not saved')
            inventory = {key: value for key,value in player.items() if key in (b'Inventory',b'equipment',b'EnderItems')}
            assert b'Inventory' in inventory
            row['players_verified'].append(dict(name=name,selected=1,
                inventory_sha256=hashlib.sha256(json.dumps(canonical(inventory)).encode()).hexdigest()))
        assert row['missing'] == row['failures'] == 0
        assert row['received_actions'] == row['placed'] + row['broken'] == row['bots'] * 90
        assert row['observed_drop_entities'] == row['bots'] * 30
    (root/'player-comparison.json').write_text(json.dumps(rows,indent=2)+'\n')
    return rows


def run(args):
    output=args.result_dir.resolve(); output.mkdir(parents=True,exist_ok=False)
    results={}
    for mode, binary in [('before',args.before_binary),('after',args.after_binary)]:
        command=[sys.executable,ROOT/'tests/stress/profile_attached.py','--binary',binary.resolve(),
            '--template',args.template.resolve(),'--result-dir',output/mode,'--bots',*map(str,args.bots),
            '--repetitions',str(args.repetitions),'--cycles','30','--fsync-probe',args.fsync_probe.resolve(),
            '--write-trace','--save-trace']
        with (output/(mode+'.log')).open('w') as log:
            subprocess.run(list(map(str,command)),cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
        results[mode]=collect(output/mode)
    for before,after in zip(results['before'],results['after']):
        for key in ('bots','repetition','cycles_per_bot','received_actions','observed_drop_entities','players_verified'):
            assert before[key]==after[key],(key,'work or final inventory differs')
    (output/'proof.json').write_text(json.dumps(dict(status='passed',same_work=True,
        exact_final_player_inventories=True,runs_per_version=len(results['after']),
        actions_per_version=sum(r['received_actions'] for r in results['after'])),indent=2)+'\n')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('before-binary','after-binary','template','result-dir','fsync-probe'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--bots',nargs='+',type=int,default=[1,2,10,25,50])
    parser.add_argument('--repetitions',type=int,default=3)
    run(parser.parse_args())
