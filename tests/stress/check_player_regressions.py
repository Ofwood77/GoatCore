#!/usr/bin/env python3
"""Remote-only persistence fix regressions, against recorded official observations.
Also invokes the existing integrated runner (including fresh Vanilla stairs).
"""
import argparse
import hashlib
import json
from pathlib import Path
import socket
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]


def port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1',0));number=sock.getsockname()[1]
    assert number not in (25565,25587)
    return number


def run(args):
    out=args.result_dir.resolve();out.mkdir(parents=True,exist_ok=False)
    def execute(label,command):
        with (out/(label+'.log')).open('w') as log:
            subprocess.run(list(map(str,command)),cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=1800)
    common=[sys.executable,ROOT/'tests/stress/run_block_checks.py','--kind','goatcore',
        '--server-binary',args.server.resolve(),'--view-distance','2','--verify-restarts','2','--shutdown-timeout','120']
    execute('vanilla-attached',[sys.executable,ROOT/'tests/stress/run_block_checks.py','--kind','vanilla',
        '--suite','attached','--port',port(),'--result-dir',out/'vanilla-attached',
        '--verify-restarts','2','--shutdown-timeout','120','--vanilla-root',args.vanilla_root.resolve(),
        '--java',args.java.resolve(),'--world-template',args.vanilla_template.resolve()])
    for label,suite,backend,extra in [
        ('attached','attached','goatworld-sqlite-experimental',[]),
        ('ladder-water','attached','goatworld-sqlite-experimental',['--attached-ladder-traps']),
        ('hinged','hinged','goatworld-sqlite-experimental',[]),
        ('bin-placement','placement','bin',['--blocks-only']),
        ('anvil-placement','placement','anvil-atomic-experimental',['--blocks-only'])]:
        mode='immediate' if backend=='bin' else 'async'
        execute(label,common+['--suite',suite,'--chunk-storage',backend,'--anvil-save-mode',mode,
            '--port',port(),'--result-dir',out/label,*extra])
    official=json.loads((ROOT/'tests/fixtures/vanilla-26.1.1-attached.json').read_text())
    fresh=json.loads((out/'vanilla-attached/report-edit.json').read_text())
    assert fresh['cases']==official['main']['cases'],'fresh Vanilla observations differ'
    for label,reference in [('attached',official['main']),('ladder-water',official['ladder_trap_water']),
        ('hinged',json.loads((ROOT/'tests/fixtures/vanilla-26.1.1-hinged.json').read_text()))]:
        actual=json.loads((out/label/'report-edit.json').read_text())
        assert actual['cases']==reference['cases'],label
    execute('integrated',[sys.executable,ROOT/'tests/stress/check_goatworld_integrated.py',
        '--server',args.server.resolve(),'--seed-tool',args.seed_tool.resolve(),'--complex-nbt',args.complex_nbt.resolve(),
        '--java',args.java.resolve(),'--vanilla-root',args.vanilla_root.resolve(),'--result-dir',out/'integrated'])
    proof=dict(status='passed',server_sha256=hashlib.sha256(args.server.read_bytes()).hexdigest(),
        attached_cases=310,hinged_cases=335,stairs_observations=192,bin_and_anvil=True,
        two_observers=True,two_clean_restarts=True,
        block_reference='recorded official 26.1.1 fixtures; main attached and stairs also rerun on official server',
        integrated=json.loads((out/'integrated/proof.json').read_text()))
    (out/'proof.json').write_text(json.dumps(proof,indent=2)+'\n');print(json.dumps(proof))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for key in ('server','seed-tool','complex-nbt','java','vanilla-root','vanilla-template','result-dir'):
        p.add_argument('--'+key,type=Path,required=True)
    run(p.parse_args())
