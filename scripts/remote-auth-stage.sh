#!/bin/sh
set -eu

remote=${GOATCORE_REMOTE:-root@10.0.4.66}
action=${1:-status}
repo_root=$(CDPATH= cd "$(dirname "$0")/.." && pwd)

case "$action" in
    start)
        cd "$repo_root"
        ssh -o BatchMode=yes "$remote" \
            'install -d -m 0755 /opt/goatcore/auth-stage /opt/goatcore/auth-stage/assets /opt/goatcore/auth-stage/tests/stress/network /opt/goatcore/auth-stage/tools'
        # Keep the production tree and user-owned untracked files out of this stage.
        tar -cf - --exclude='*.o' --exclude='*.d' --exclude='src/main.py' \
            src include Makefile \
            assets/registry_packets_26_1_1.bin assets/tags_packet_26_1_1.bin \
            assets/chunk_0_0_26_1_1.bin assets/block_states.json \
            tests/test_auth.c tests/test_login_online.c tests/test_framing_crypto.c \
            tests/test_profile.c \
            tests/test_login_integration.py tests/test_online_negative_live.py \
            tests/stress/stress.py tests/stress/protocol.py tests/stress/network/one.conf \
            tools/mc_ping.py \
            | ssh -o BatchMode=yes "$remote" 'cd /opt/goatcore/auth-stage && tar -xf -'
        scp -q -o BatchMode=yes config/auth-stage.server.config \
            "$remote:/opt/goatcore/auth-stage/server.config"
        scp -q -o BatchMode=yes scripts/goatcore-auth-stage.service \
            "$remote:/etc/systemd/system/goatcore-auth-stage.service"
        ssh -o BatchMode=yes "$remote" \
            'set -eu; cd /opt/goatcore/auth-stage; make -B -j4 mc_server test_auth test_login_online test_framing_crypto test_profile; ./test_auth; ./test_login_online; ./test_framing_crypto; ./test_profile; install -d -o goatcore -g goatcore -m 0755 /var/lib/goatcore/auth-stage-world; chown -R goatcore:goatcore /opt/goatcore/auth-stage; systemctl daemon-reload; systemctl restart goatcore-auth-stage.service; for attempt in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do if systemctl is-active --quiet goatcore-auth-stage.service && ss -lnt | grep -q ":25588 "; then exit 0; fi; sleep 0.2; done; journalctl -u goatcore-auth-stage.service -n 30 --no-pager; exit 1'
        echo "GoatCore auth stage ready on $remote:25588; 25565 and 25587 unchanged."
        ;;
    status)
        exec ssh -o BatchMode=yes "$remote" 'systemctl --no-pager --full status goatcore-auth-stage.service'
        ;;
    logs)
        exec ssh -o BatchMode=yes "$remote" 'journalctl -u goatcore-auth-stage.service -n 80 --no-pager'
        ;;
    stop)
        ssh -o BatchMode=yes "$remote" 'systemctl stop goatcore-auth-stage.service'
        echo "GoatCore auth stage stopped; 25565 and 25587 unchanged."
        ;;
    *)
        echo "Usage: $0 {start|status|logs|stop}" >&2
        exit 2
        ;;
esac
