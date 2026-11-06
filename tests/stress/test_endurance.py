#!/usr/bin/env python3
"""No server/client startup: meaningful guard, retention and packet bounds checks."""
import asyncio
import gzip
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch
import signal
import endurance_server
from endurance_common import Rotating, atomic_json, protection
from endurance_bots import ActiveBot, lane
from collections import Counter
from protocol import position,varint


class EnduranceTests(unittest.TestCase):
    def test_guards(self):
        limits=dict(public_latency_ms=250,minimum_available_mib=2048,minimum_disk_free_mib=8192,
            maximum_world_mib=8192,maximum_wal_mib=512,memory_full_avg10=1,io_full_avg10=20)
        safe=dict(public_unchanged=True,public_latency_ms=1,available_mib=32768,disk_free_mib=40000,
            world_bytes=1,wal_bytes=1,oom=False,memory_full_avg10=0,io_full_avg10=0,worker_stale=False,storage_errors=False)
        self.assertEqual(protection(safe,limits),[])
        for field,value,label in [('public_unchanged',False,'public-service-changed'),('public_latency_ms',251,'public-status-latency'),
            ('available_mib',2047,'available-memory'),('disk_free_mib',8191,'disk-free'),('world_bytes',8192*1048576+1,'world-growth'),
            ('wal_bytes',512*1048576+1,'wal-growth'),('oom',True,'cgroup-oom'),('memory_full_avg10',2,'memory-pressure'),
            ('io_full_avg10',21,'io-pressure'),('worker_stale',True,'generator-stale'),('storage_errors',True,'storage-error')]:
            self.assertEqual(protection(dict(safe,**{field:value}),limits),[label])

    def test_rotation_retains_newest(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'events';sink=Rotating(path,maximum=16,backups=2)
            for number in range(12):sink.write(f'{number:02d}abcd\n')
            sink.close()
            files=list(Path(folder).iterdir());self.assertEqual(len(files),3)
            content=gzip.decompress(Path(str(path)+'.2.gz').read_bytes())+gzip.decompress(Path(str(path)+'.1.gz').read_bytes())+path.read_bytes()
            self.assertEqual(content,b''.join(f'{i:02d}abcd\n'.encode() for i in range(6,12)))
            atomic_json(Path(folder)/'status.json',{'active':5});self.assertFalse(list(Path(folder).glob('*.tmp')))

    def test_unique_shared_world_lanes(self):
        self.assertEqual(len({lane(i) for i in range(250)}),250)
        self.assertTrue(any(lane(i)[0]<0 for i in range(250)))

    def test_unrelated_updates_do_not_accumulate(self):
        async def check():
            bot=ActiveBot(0,SimpleNamespace(port=25587),Counter())
            for i in range(10000):await bot.play_packet(8,position(i,80,0)+varint(1))
            self.assertEqual(bot.states,{})
            await bot.play_packet(8,position(*bot.target)+varint(7));self.assertEqual(bot.states,{bot.target:7})
            await bot.play_packet(4,varint(12));self.assertEqual(bot.ack,12)
            self.assertTrue(callable(bot.query_state));self.assertEqual(bot.state,'new')
        asyncio.run(check())

    def test_server_reaped_if_evidence_write_fails(self):
        # No real process or Minecraft server: exercise the wrapper's failure
        # cleanup after a mocked child has been spawned.
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            for kind in ('ticks','saves'):(root/(kind+'.fifo')).touch()
            child=Mock();child.poll.return_value=None;child.stdout=[]
            with patch.object(endurance_server.threading.Thread,'start'), \
                 patch.object(endurance_server.subprocess,'Popen',return_value=child), \
                 patch.object(endurance_server.signal,'signal'), \
                 patch.object(endurance_server,'atomic_json',side_effect=OSError('injected EIO')):
                with self.assertRaises(OSError):
                    endurance_server.run(SimpleNamespace(campaign=root,binary=root/'fake'))
            child.send_signal.assert_called_once_with(signal.SIGTERM)
            child.wait.assert_called_once_with(timeout=150)


if __name__=='__main__':unittest.main()
