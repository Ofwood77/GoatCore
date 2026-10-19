#!/bin/sh
set -eu

remote=${1:-root@10.0.4.66}
repo_root=$(CDPATH= cd "$(dirname "$0")/.." && pwd)

cd "$repo_root"
ssh -o BatchMode=yes "$remote" 'install -d -m 0755 /opt/goatcore/app/assets /opt/goatcore/app/tests/stress /etc/goatcore'

# Transfer the current working tree, including uncommitted source changes, but
# never a local world, build output, virtual environment, or stress reports.
tar -cf - --exclude='*.o' --exclude='*.d' --exclude='__pycache__' \
    --exclude='results' src include Makefile \
    assets/registry_packets_26_1_1.bin assets/tags_packet_26_1_1.bin \
    assets/chunk_0_0_26_1_1.bin assets/block_states.json tests/stress \
    | ssh -o BatchMode=yes "$remote" 'cd /opt/goatcore/app && tar -xf -'
scp -q -o BatchMode=yes scripts/goatcore.service scripts/goatcore-test.service \
    "$remote:/etc/systemd/system/"
scp -q -o BatchMode=yes config/server1.setup.conf "$remote:/etc/goatcore/server1.setup.conf.dist"
scp -q -o BatchMode=yes config/server2.setup.conf "$remote:/etc/goatcore/server2.setup.conf.dist"

ssh -o BatchMode=yes "$remote" \
    'set -eu; id -u goatcore >/dev/null 2>&1 || useradd --system --home-dir /var/lib/goatcore --shell /usr/sbin/nologin goatcore; test -e /etc/goatcore/server1.setup.conf || cp /etc/goatcore/server1.setup.conf.dist /etc/goatcore/server1.setup.conf; test -e /etc/goatcore/server2.setup.conf || cp /etc/goatcore/server2.setup.conf.dist /etc/goatcore/server2.setup.conf; test_was_active=0; if systemctl is-active --quiet goatcore-test.service; then test_was_active=1; fi; cd /opt/goatcore/app; make -j4 mc_server; chown -R goatcore:goatcore /opt/goatcore/app; systemctl daemon-reload; systemctl enable goatcore.service; systemctl restart goatcore.service; if [ "$test_was_active" -eq 1 ]; then systemctl restart goatcore-test.service; fi; systemctl is-active goatcore.service'

echo "GoatCore deployee sur $remote (service goatcore.service)."
