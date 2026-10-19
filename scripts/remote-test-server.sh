#!/bin/sh
set -eu

remote=${GOATCORE_REMOTE:-root@10.0.4.66}
action=${1:-status}

case "$action" in
    start)
        ssh -o BatchMode=yes "$remote" \
            'systemctl start goatcore-test.service && systemctl is-active goatcore-test.service'
        echo "Serveur de test : $remote:25587 (monde distinct du serveur normal)."
        ;;
    stop)
        ssh -o BatchMode=yes "$remote" 'systemctl stop goatcore-test.service'
        echo "Serveur de test arrete ; serveur normal 25565 inchange."
        ;;
    status)
        ssh -o BatchMode=yes "$remote" 'systemctl --no-pager --full status goatcore-test.service'
        ;;
    logs)
        exec ssh -t -o BatchMode=yes "$remote" 'journalctl -fu goatcore-test.service'
        ;;
    *)
        echo "Usage: $0 {start|stop|status|logs}" >&2
        exit 2
        ;;
esac
