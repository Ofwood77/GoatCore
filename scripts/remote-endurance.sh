#!/bin/sh
# No command here targets goatcore.service or 25565.
set -eu
remote=${GOATCORE_REMOTE:-root@10.0.4.66}
case ${1:-status} in
 start) ssh -o BatchMode=yes "$remote" 'systemctl start goatcore-endurance.service' ;;
 status) ssh -o BatchMode=yes "$remote" 'systemctl show goatcore.service goatcore-test.service goatcore-endurance.service -p Id -p ActiveState -p MainPID; cat /var/lib/goatcore/endurance/current/metrics/latest.json; test ! -f /var/lib/goatcore/endurance/current/metrics/final.json || cat /var/lib/goatcore/endurance/current/metrics/final.json' ;;
 stop) ssh -o BatchMode=yes "$remote" 'touch /var/lib/goatcore/endurance/current/metrics/STOP; systemctl stop goatcore-endurance.service; systemctl stop goatcore-test.service' ;;
 collect)
  target=${2:?local NEW output directory}; mkdir "$target"
  # Bounded metric/log tree only, never world/player files or credentials.
  ssh -o BatchMode=yes "$remote" 'tar -C /var/lib/goatcore/endurance/current --exclude="*.fifo" --exclude="*.tmp" -cf - config.json metrics' | tar -xf - -C "$target"
  ;;
 *) echo "Usage: $0 {start|status|stop|collect NEW_LOCAL_DIRECTORY}" >&2; exit 2 ;;
esac
