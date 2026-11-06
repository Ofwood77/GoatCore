"""Bounded evidence and pure protection rules; no Minecraft server startup."""
import gzip
import json
import os
from pathlib import Path
import shutil
import time


def utc():
    return time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())


def atomic_json(path, value):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, sort_keys=True) + '\n')
    temporary.replace(path)


class Rotating:
    """Single-owner stream; compression/retention bounded, never copytruncate."""
    def __init__(self, path, maximum=8*1024*1024, backups=3):
        self.path = Path(path)
        self.maximum, self.backups = maximum, backups
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.file = self.path.open('a')
        self.size = self.path.stat().st_size

    def write(self, text):
        if self.size + len(text.encode()) > self.maximum:
            self.file.close()
            for index in range(self.backups, 0, -1):
                previous = Path(str(self.path) + f'.{index}.gz')
                if index == self.backups: previous.unlink(missing_ok=True)
                elif previous.exists(): previous.replace(str(self.path) + f'.{index+1}.gz')
            with self.path.open('rb') as source, gzip.open(str(self.path)+'.1.gz', 'wb') as target:
                shutil.copyfileobj(source, target, 65536)
            self.file = self.path.open('w')
            self.size = 0
        self.file.write(text)
        self.file.flush()
        self.size += len(text.encode())

    def close(self):
        self.file.close()


def proc(pid):
    root = Path('/proc')/str(pid)
    stat = (root/'stat').read_text().split(') ', 1)[1].split()
    status = {}
    for line in (root/'status').read_text().splitlines():
        key, _, value = line.partition(':')
        if key in ('VmRSS', 'VmHWM', 'VmSize', 'VmSwap', 'Threads'):
            status[key] = value.strip()
    io = dict(line.split(': ') for line in (root/'io').read_text().splitlines())
    return dict(pid=pid, cpu_s=(int(stat[11])+int(stat[12]))/os.sysconf('SC_CLK_TCK'),
        status=status, io={k:int(v) for k,v in io.items()},
        threads={p.parent.name:sum(map(int,p.read_text().split(') ',1)[1].split()[11:13]))/os.sysconf('SC_CLK_TCK')
            for p in root.glob('task/*/stat')})


def protection(observed, limits):
    reasons = []
    if not observed['public_unchanged']: reasons.append('public-service-changed')
    if observed['public_latency_ms'] > limits['public_latency_ms']: reasons.append('public-status-latency')
    if observed['available_mib'] < limits['minimum_available_mib']: reasons.append('available-memory')
    if observed['disk_free_mib'] < limits['minimum_disk_free_mib']: reasons.append('disk-free')
    if observed['world_bytes'] > limits['maximum_world_mib']*1048576: reasons.append('world-growth')
    if observed['wal_bytes'] > limits['maximum_wal_mib']*1048576: reasons.append('wal-growth')
    if observed['oom']: reasons.append('cgroup-oom')
    if observed['memory_full_avg10'] > limits['memory_full_avg10']: reasons.append('memory-pressure')
    if observed['io_full_avg10'] > limits['io_full_avg10']: reasons.append('io-pressure')
    if observed['worker_stale']: reasons.append('generator-stale')
    if observed['storage_errors']: reasons.append('storage-error')
    return reasons
