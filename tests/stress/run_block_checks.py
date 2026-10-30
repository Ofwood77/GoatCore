#!/usr/bin/env python3
"""Remote-host runner for isolated GoatCore/Vanilla block checks and restart.

Run exclusively on the dedicated host. Never touches the two main services.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time
import zlib
import gzip

ROOT=Path(__file__).resolve().parents[2]


def region_payloads(path):
    data=path.read_bytes()
    assert len(data)>=8192 and len(data)%4096==0
    payloads={}
    for i in range(1024):
        entry=int.from_bytes(data[i*4:i*4+4],'big')
        if not entry:continue
        offset,count=entry>>8,entry&255
        assert offset>=2 and count and (offset+count)*4096<=len(data)
        pos=offset*4096;length=int.from_bytes(data[pos:pos+4],'big');kind=data[pos+4]
        assert 1<=length<=count*4096-4 and kind in (1,2,3), (path,i,kind)
        raw=data[pos+5:pos+4+length]
        if kind==1:raw=gzip.decompress(raw)
        if kind==2:raw=zlib.decompress(raw)
        payloads[i]=hashlib.sha256(raw).hexdigest()
    return payloads


def rewrite_regions(runtime, probe, output, s2_experimental=False):
    rows=[]
    region_dir=runtime/'world/dimensions/minecraft/overworld/region'
    if not region_dir.is_dir():region_dir=runtime/'world/region'
    if s2_experimental:
        # This runtime was freshly created by this runner; never a supplied world.
        marker=region_dir/'.goatcore-s2-disposable'
        if not marker.exists():
            with marker.open('x') as file:
                file.write('GoatCore S2 disposable tests only\n');file.flush();os.fsync(file.fileno())
            directory=os.open(region_dir,os.O_RDONLY|os.O_DIRECTORY|os.O_CLOEXEC)
            try:os.fsync(directory)
            finally:os.close(directory)
    for path in sorted(region_dir.glob('*.mca')):
        before=region_payloads(path)
        if not before:continue
        slot=next(iter(before))
        prefix=[str(probe.resolve())]+(['--experimental-disposable'] if s2_experimental else [])
        done=subprocess.run(prefix+['rewrite-region',str(path),str(slot%32),str(slot//32)],
                            capture_output=True,text=True,timeout=120,check=True)
        after=region_payloads(path)
        assert before==after, path
        rows.append(dict(region=path.name,slot=slot,chunks=len(before),payload_sha256=after,
                         writer=json.loads(done.stdout)))
    assert rows, 'no native Vanilla region rewritten'
    output.write_text(json.dumps(rows,indent=2))


def run(args):
    if args.port in (25565,25587): raise ValueError("main service ports are forbidden")
    if args.fsync_probe and not args.fsync_probe.is_file():raise ValueError("fsync probe library not found")
    if args.s2_experimental and not args.anvil_rewriter:raise ValueError('S2 requires an explicit experimental rewriter')
    if args.anvil_rewriter and args.kind!='vanilla':raise ValueError("native region rewrite requires a disposable Vanilla world")
    (ROOT/"tests/stress/results").mkdir(parents=True,exist_ok=True)
    result=args.result_dir.resolve() if args.result_dir else Path(tempfile.mkdtemp(prefix="blocks-",dir=ROOT/"tests/stress/results"))
    if args.result_dir:result.mkdir(parents=True,exist_ok=False)
    runtime=result/"runtime";runtime.mkdir()
    if args.kind=="vanilla":
        reference=args.vanilla_root.resolve()
        for name in ("server.jar","libraries","versions"):(runtime/name).symlink_to(reference/name)
        (runtime/"eula.txt").write_text("eula=true\n")
        (runtime/"server.properties").write_text(f"server-port={args.port}\nonline-mode=false\nenforce-secure-profile=false\ngamemode=creative\nlevel-type=minecraft:flat\nlevel-seed=0\nspawn-protection=0\nview-distance=4\nsimulation-distance=4\ndifficulty=peaceful\n")
        (runtime/"ops.json").write_text(json.dumps([{"uuid":"3dbc2475-f96a-3b21-8dae-78a0715c5e2f","name":"Ofwood_","level":4,"bypassesPlayerLimit":False}]))
        java=args.java.resolve() if args.java else reference/"jdk-25.0.5+8/bin/java"
        command=[str(java),"-Xms256M","-Xmx1G","-jar","server.jar","nogui"]
        env=os.environ.copy()
    else:
        (runtime/"assets").symlink_to(ROOT/"assets")
        (runtime/"server.config").write_text("online-mode=false\noffline-skins=false\n")
        env=dict(os.environ,MC_WORLD_PATH=str(runtime/"world"),MC_BIND_PORT=str(args.port),
                 MC_SETUP_FILE=str(ROOT/"tests/stress/network/one.conf"),MC_QA_MODE="1",MC_ADMIN_USERNAME="Ofwood_")
        command=[str(args.server_binary.resolve())]
    for phase in ["edit", "verify"] + [f"verify-{i}" for i in range(2, args.verify_restarts + 1)]:
        if phase!='edit' and args.anvil_rewriter:
            rewrite_regions(runtime,args.anvil_rewriter,result/f'anvil-rewrite-{phase}.json',args.s2_experimental)
        phase_env=env.copy()
        if args.fsync_probe and args.kind=="goatcore":
            phase_env.update(MC_PERF="1",MC_PERF_TRACE_PATH=str(result/f"ticks-{phase}.csv"),
                             LD_PRELOAD=str(args.fsync_probe.resolve()),MC_FSYNC_TRACE_PATH=str(result/f"fsync-{phase}.csv"))
        with (result/f"server-{phase}.log").open("w") as log:
            proc=subprocess.Popen(command,cwd=runtime,env=phase_env,stdin=subprocess.PIPE,stdout=log,stderr=subprocess.STDOUT,text=True)
            try:
                deadline=time.monotonic()+90
                while True:
                    if proc.poll() is not None:raise RuntimeError(f"server startup exit {proc.returncode}")
                    try:
                        with socket.create_connection(("127.0.0.1",args.port),timeout=.2):break
                    except ConnectionRefusedError:
                        if time.monotonic()>deadline:raise TimeoutError("server readiness")
                        time.sleep(.1)
                scenario={"stairs":"test_stair_placement.py", "placement":"test_block_placement.py", "pickup":"test_pickup_save.py"}[args.suite]
                cmd=[sys.executable,str(ROOT/"tests/stress"/scenario),
                     "--port",str(args.port),"--snapshot",str(result/"snapshot.json")]
                if args.kind=="vanilla":cmd.append("--vanilla")
                if args.suite=="pickup":cmd.extend(["--drop-count",str(args.pickup_count)])
                if phase!="edit":cmd.append("--verify")
                test=subprocess.run(cmd,cwd=ROOT,capture_output=True,text=True,timeout=600)
                (result/f"test-{phase}.log").write_text(test.stdout+test.stderr)
                if test.returncode:raise RuntimeError(f"{phase} checks failed; see {result}/test-{phase}.log")
                report=json.loads(test.stdout.strip())
                (result/f"report-{phase}.json").write_text(json.dumps(report,indent=2))
            finally:
                if proc.poll() is None:
                    if args.kind=="vanilla":proc.stdin.write("stop\n");proc.stdin.flush()
                    else:proc.send_signal(signal.SIGTERM)
                try:proc.wait(timeout=60)
                except subprocess.TimeoutExpired:
                    proc.kill();proc.wait();raise RuntimeError("temporary server killed after shutdown timeout")
            if proc.returncode:raise RuntimeError(f"server shutdown exit {proc.returncode}")
            if args.fsync_probe and args.kind=="goatcore" and not (result/f"fsync-{phase}.csv").is_file():
                raise RuntimeError("fsync probe did not produce a trace")
    metadata={"kind":args.kind,"suite":args.suite,"port":args.port,"status":"passed","verify_restarts":args.verify_restarts}
    if args.kind=="goatcore":metadata["binary_sha256"]=hashlib.sha256(args.server_binary.read_bytes()).hexdigest()
    (result/"metadata.json").write_text(json.dumps(metadata,indent=2))
    print(json.dumps({**metadata,"results":str(result)}))


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kind",choices=("goatcore","vanilla"),required=True)
    parser.add_argument("--suite",choices=("stairs","placement","pickup"),default="stairs")
    parser.add_argument("--port",type=int,required=True)
    parser.add_argument("--server-binary",type=Path,default=ROOT/"mc_server")
    parser.add_argument("--vanilla-root",type=Path,default=Path("/opt/goatcore/vanilla-2611"))
    parser.add_argument("--java",type=Path,help="Java 25 executable for an isolated Vanilla installation")
    parser.add_argument("--result-dir",type=Path)
    parser.add_argument("--pickup-count",type=int,default=2,choices=(2,100))
    parser.add_argument("--verify-restarts",type=int,default=1,choices=range(1,11),help="successive clean restarts of the same world")
    parser.add_argument("--s2-experimental",action="store_true",help="arm S2 only inside this fresh isolated Vanilla runtime")
    parser.add_argument("--anvil-rewriter",type=Path,help="rewrite native Vanilla regions with mc_save_bench before each verification restart")
    parser.add_argument("--fsync-probe",type=Path,help="optional test-only fsync library and per-phase tick traces")
    run(parser.parse_args())
