#!/bin/sh
set -eu

remote=${GOATCORE_REMOTE:-root@10.0.4.66}
if [ "$#" -gt 3 ]; then
    echo "Usage: $0 [nombre:1..1000] [connect|movement|blocks|mixed|fastblocks] [duree:1..3600]" >&2
    exit 2
fi
bots=${1:-5}
mode=${2:-movement}
duration=${3:-30}

case "$bots" in
    ''|*[!0-9]*) echo "Nombre de bots invalide: $bots" >&2; exit 2 ;;
esac
if [ "$bots" -lt 1 ] || [ "$bots" -gt 1000 ]; then
    echo "Nombre de bots attendu: 1..1000" >&2
    exit 2
fi

case "$duration" in
    ''|*[!0-9]*) echo "Duree invalide: $duration" >&2; exit 2 ;;
esac
if [ "$duration" -lt 1 ] || [ "$duration" -gt 3600 ]; then
    echo "Duree attendue: 1..3600 secondes" >&2
    exit 2
fi

case "$mode" in
    connect|movement) extra='' ;;
    blocks|mixed)
        if [ "$bots" -gt 50 ]; then
            echo "Les modes blocs sont limites a 50 cibles distinctes autour du spawn" >&2
            exit 2
        fi
        extra='--allow-block-changes' ;;
    fastblocks)
        if [ "$bots" -gt 50 ]; then
            echo "Le mode fastblocks est limite a 50 cibles distinctes autour du spawn" >&2
            exit 2
        fi
        extra='--allow-block-changes --block-rate 400' ;;
    *) echo "Mode attendu: connect, movement, blocks, mixed ou fastblocks" >&2; exit 2 ;;
esac

ssh -o BatchMode=yes "$remote" \
    "systemctl is-active --quiet goatcore-test.service && cd /opt/goatcore/app && runuser -u goatcore -- python3 tests/stress/stress.py --host 127.0.0.1 --port 25587 --bots $bots --mode $mode --duration $duration --spawn-delay 0.02 $extra"
