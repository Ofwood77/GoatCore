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

ROOT=Path(__file__).resolve().parents[2]


def run(args):
    if args.port in (25565,25587): raise ValueError("main service ports are forbidden")
    if args.fsync_probe and not args.fsync_probe.is_file():raise ValueError("fsync probe library not found")
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
    parser.add_argument("--fsync-probe",type=Path,help="optional test-only fsync library and per-phase tick traces")
    run(parser.parse_args())
