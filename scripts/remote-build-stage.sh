#!/bin/sh
# Export sources into a new isolated directory; never touch either main service.
set -eu
remote=${GOATCORE_REMOTE:-root@10.0.4.66}
repo_root=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
cd "$repo_root"
./tools/check_repo_hygiene.sh >&2
stage=$(ssh -o BatchMode=yes "$remote" 'mktemp -d /opt/goatcore/phase2a-stage.XXXXXX')
# Same source-export mechanism as remote-auth-stage.sh, extended for full checks.
tar -cf - --exclude='*.o' --exclude='*.d' --exclude='__pycache__' \
    --exclude='src/main.py' --exclude='results' --exclude='node_modules' src include Makefile tests tools \
    assets/registry_packets_26_1_1.bin assets/tags_packet_26_1_1.bin \
    assets/chunk_0_0_26_1_1.bin assets/block_states.json \
    | ssh -o BatchMode=yes "$remote" "cd '$stage' && tar -xf -"
# blocks.json was checked against the official 26.1.1 datagen report.
if [ -f data/26.1.1/reports/blocks.json ]; then
    ssh -o BatchMode=yes "$remote" "mkdir -p '$stage/data/26.1.1/reports'"
    scp -q data/26.1.1/reports/blocks.json "$remote:$stage/data/26.1.1/reports/blocks.json"
    if [ -f data/26.1.1/reports/registries.json ]; then
        scp -q data/26.1.1/reports/registries.json "$remote:$stage/data/26.1.1/reports/registries.json"
    fi
fi
printf '%s\n' "$stage"
