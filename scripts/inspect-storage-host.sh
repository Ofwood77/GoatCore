#!/bin/sh
# Read-only S5 preflight. No deployment, service mutation or Git on the host.
set -eu
remote=${GOATCORE_REMOTE:-root@10.0.4.66}
exec ssh -o BatchMode=yes -o ConnectTimeout=6 -o ConnectionAttempts=1 "$remote" 'sh -s' <<'REMOTE'
date -u
uname -a
systemd-detect-virt || true
systemctl show goatcore.service goatcore-test.service \
    -p Id -p ActiveState -p SubState -p MainPID -p ExecMainStatus
uptime
free -m
df -h
lsblk -o NAME,TYPE,SIZE,ROTA,MOUNTPOINTS
cat /proc/mdstat
cat /proc/pressure/io
cat /proc/pressure/cpu
ps -eo pid,comm,pcpu,pmem --sort=-pcpu | head -n 16
ss -ltn
# Device names/RAID visibility may be restricted by the container. Absence here
# is not evidence of physical storage health or durability.
REMOTE
