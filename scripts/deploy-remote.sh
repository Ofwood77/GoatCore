#!/bin/sh
set -eu

remote=${1:-root@10.0.4.66}
repo_root=$(CDPATH= cd "$(dirname "$0")/.." && pwd)

cd "$repo_root"
ssh -o BatchMode=yes "$remote" 'install -d -m 0755 /opt/goatcore/app/assets /opt/goatcore/app/tests/stress /opt/goatcore/app/instances/online /opt/goatcore/app/instances/offline /etc/goatcore'

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
scp -q -o BatchMode=yes config/server-online.config "$remote:/etc/goatcore/server-online.config.dist"
scp -q -o BatchMode=yes config/server-offline.config "$remote:/etc/goatcore/server-offline.config.dist"

ssh -o BatchMode=yes "$remote" \
    'set -eu
    id -u goatcore >/dev/null 2>&1 || useradd --system --home-dir /var/lib/goatcore --shell /usr/sbin/nologin goatcore
    test -e /etc/goatcore/server1.setup.conf || cp /etc/goatcore/server1.setup.conf.dist /etc/goatcore/server1.setup.conf
    test -e /etc/goatcore/server2.setup.conf || cp /etc/goatcore/server2.setup.conf.dist /etc/goatcore/server2.setup.conf
    for instance in online offline; do
        instance_dir=/opt/goatcore/app/instances/$instance
        test -e "$instance_dir/assets" || ln -s ../../assets "$instance_dir/assets"
        if ! test -e "$instance_dir/server.config"; then
            if test -f /opt/goatcore/app/server.config; then
                cp /opt/goatcore/app/server.config "$instance_dir/server.config"
            else
                cp "/etc/goatcore/server-$instance.config.dist" "$instance_dir/server.config"
            fi
            if test "$instance" = online; then mode=true; else mode=false; fi
            if grep -q "^online-mode=" "$instance_dir/server.config"; then
                sed -i "s/^online-mode=.*/online-mode=$mode/" "$instance_dir/server.config"
            else
                printf "\nonline-mode=%s\n" "$mode" >> "$instance_dir/server.config"
            fi
        fi
    done
    grep -qx "online-mode=true" /opt/goatcore/app/instances/online/server.config
    grep -qx "online-mode=false" /opt/goatcore/app/instances/offline/server.config
    test_was_active=0
    if systemctl is-active --quiet goatcore-test.service; then test_was_active=1; fi
    cd /opt/goatcore/app
    make -j4 mc_server
    chown -R goatcore:goatcore /opt/goatcore/app
    systemctl daemon-reload
    systemctl enable goatcore.service
    systemctl restart goatcore.service
    if [ "$test_was_active" -eq 1 ]; then systemctl restart goatcore-test.service; fi
    systemctl is-active goatcore.service'

echo "GoatCore deployee sur $remote (service goatcore.service)."
