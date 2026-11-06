#!/bin/bash
# Explicitly authorized OFFLINE test-service installation. Never replace app/mc_server.
set -euo pipefail
stage=${1:?remote staging directory}; campaign=${2:?campaign ID}; revision=${3:?published/source SHA}; watch=${4:-false}
[[ $stage =~ ^/opt/goatcore/phase2a-stage\.[A-Za-z0-9]+$ && $campaign =~ ^[a-z0-9-]+$ && $revision =~ ^[0-9a-f]{40}$ && $watch =~ ^(true|false)$ ]] || exit 2
remote=${GOATCORE_REMOTE:-root@10.0.4.66}
# Send literal arguments, constrained above; no credential files are transferred.
ssh -o BatchMode=yes "$remote" bash -s -- "$stage" "$campaign" "$revision" "$watch" <<'REMOTE'
set -euo pipefail
stage=$1; campaign=$2; revision=$3; watch=$4
release=/opt/goatcore/endurance/releases/$campaign
root=/var/lib/goatcore/endurance/$campaign
before=$(sha256sum /opt/goatcore/app/mc_server /opt/goatcore/app/instances/online/server.config /etc/goatcore/server1.setup.conf /etc/systemd/system/goatcore.service)
[[ $(systemctl is-active goatcore.service) == active ]]
public_state=$(systemctl show goatcore.service -p MainPID -p NRestarts -p ActiveState)
# Stop only a previous endurance controller/test instance, never 25565.
if systemctl is-active --quiet goatcore-endurance.service; then systemctl stop goatcore-endurance.service; fi
systemctl stop goatcore-test.service
[[ ! -e $release && ! -e $root ]]
mkdir -p "$release/tests/stress" "$release/src/generated" "$release/data/26.1.1/reports" "$root/runtime" "$root/metrics"
cp "$stage/mc_server" "$release/mc_server"
cp "$stage"/tests/stress/*.py "$release/tests/stress/"
cp "$stage/src/generated/generated_minecraft_ids.json" "$release/src/generated/"
cp "$stage/data/26.1.1/reports/blocks.json" "$release/data/26.1.1/reports/"
cp -a "$stage/assets" "$release/assets"
ln -s "$release/assets" "$root/runtime/assets"
cat > "$root/runtime/server.config" <<'CONFIG'
memory-budget-mib=8192
online-mode=false
offline-skins=false
chunk-storage=goatworld-sqlite-experimental
chunk-save-mode=async
level-seed=0
view-distance=2
simulation-distance=2
difficulty=normal
pvp=true
keep_inventory=false
CONFIG
cat > "$release/setup.conf" <<'CONFIG'
[performance]
cpu_cores = 4
cpu_affinity = 4,5,12,13
network_workers = 2
chunk_workers = 2
tick_rate = 20
CONFIG
sha=$(sha256sum "$release/mc_server" | cut -d' ' -f1)
python3 - "$root" "$release" "$revision" "$sha" "$watch" <<'PY'
import json,pathlib,sys
root,release,revision,sha,watch=sys.argv[1:]
config=dict(campaign=root+'/metrics',binary=release+'/mc_server',binary_sha256=sha,world=root+'/runtime/world',
 published_sha=revision,watch_origin=watch=='true',port=25587,generator_unit='goatcore-endurance.service',
 ramp=[5,25,50,100,150,200,250],plateau_s=45,plateau_timeout_s=300,period_s=40,admission_interval_s=1,
 limits=dict(public_latency_ms=250,minimum_available_mib=2048,minimum_disk_free_mib=8192,
 maximum_world_mib=8192,maximum_wal_mib=512,memory_full_avg10=1,io_full_avg10=20))
pathlib.Path(root+'/config.json').write_text(json.dumps(config,indent=2)+'\n')
PY
chown -R goatcore:goatcore "$root"
mkdir -p /etc/systemd/system/goatcore-test.service.d
cat > /etc/systemd/system/goatcore-test.service.d/endurance.conf <<CONFIG
[Service]
ExecStart=
ExecStart=/usr/bin/python3 $release/tests/stress/endurance_server.py --binary $release/mc_server --campaign $root/metrics
WorkingDirectory=$root/runtime
Environment=MC_WORLD_PATH=$root/runtime/world
Environment=MC_SETUP_FILE=$release/setup.conf
Environment=MC_BIND_PORT=25587
Environment=MC_ADMIN_USERNAME=Ofwood_
Environment=MC_QA_MODE=1
Environment=MC_ADMIN_COMMANDS=1
Environment=MC_MAX_CONNECTIONS=512
MemoryMax=8G
MemorySwapMax=0
CPUQuota=200%
CPUAffinity=4 5 12 13
Nice=10
IOWeight=10
IOSchedulingClass=best-effort
IOSchedulingPriority=7
TimeoutStopSec=180
KillMode=control-group
Restart=no
StandardOutput=null
StandardError=journal
CONFIG
cat > /etc/systemd/system/goatcore-endurance.service <<CONFIG
[Unit]
Description=GoatCore supervised offline endurance (25587 only)
After=goatcore-test.service
[Service]
Type=simple
ExecStart=/usr/bin/python3 $release/tests/stress/endurance_control.py --config $root/config.json
MemoryMax=2G
MemorySwapMax=0
CPUQuota=200%
CPUAffinity=6 7 14 15
Nice=15
IOWeight=10
IOSchedulingClass=best-effort
IOSchedulingPriority=7
TimeoutStopSec=240
KillMode=mixed
Restart=no
StandardOutput=null
StandardError=journal
CONFIG
ln -sfn "$root" /var/lib/goatcore/endurance/current
systemctl daemon-reload
systemctl start goatcore-test.service
python3 - <<'PYREADY'
import socket,time
for _ in range(300):
    try:
        with socket.create_connection(('127.0.0.1',25587),.1):break
    except OSError:time.sleep(.1)
else:raise TimeoutError('25587 did not become ready')
PYREADY
[[ $(systemctl is-active goatcore-test.service) == active ]]
[[ "$before" == "$(sha256sum /opt/goatcore/app/mc_server /opt/goatcore/app/instances/online/server.config /etc/goatcore/server1.setup.conf /etc/systemd/system/goatcore.service)" ]]
[[ "$public_state" == "$(systemctl show goatcore.service -p MainPID -p NRestarts -p ActiveState)" ]]
printf 'Test release: %s\nCampaign: %s\nBinary SHA256: %s\n' "$release" "$root" "$sha"
REMOTE
