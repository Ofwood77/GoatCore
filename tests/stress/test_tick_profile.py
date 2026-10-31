"""Pure unit checks for continuous measurement boundaries, not server tests."""
import csv
import asyncio
import os
import subprocess
from types import SimpleNamespace
from protocol import varint
import tempfile
import unittest
from pathlib import Path
from profile_tick import summarize, quantile, read_trace, io_summary, ProfileBot, proc_sample
from profile_save_batches import publication_window


class ContinuousProfileTest(unittest.TestCase):
    def test_backend_marker_sync_is_not_a_region_publication(self):
        def span(name,start,end):return dict(phase=name,start_us=start,end_us=end,result=0)
        rows=[span('atomic_file_sync',1,2),span('atomic_directory_sync',3,4),
              span('atomic_file_sync',20,25),span('atomic_directory_sync',26,29),
              span('region_total',10,30),span('storage_load',40,50)]
        selected=publication_window(rows)
        self.assertEqual(sum(r['phase']=='atomic_file_sync' for r in selected),1)
        self.assertEqual(sum(r['phase']=='atomic_directory_sync' for r in selected),1)

    def row(self, idx, start, end):
        names=('tasks_us fast_us world_us proto_us items_us remote_us updates_us evict_us '
               'handlers_us flush_us world_save_us integrate_us items_lock_us furnace_lock_us '
               'cpu_us interval_cpu_us task_depth task_peak task_wait_max_us items ready').split()
        return dict(dict.fromkeys(names,0),idx=idx,start_us=start,end_us=end,ready=10)

    def test_slow_crossing_tail_is_not_discarded(self):
        rows=[self.row(0,0,10000),self.row(1,50000,60000),self.row(2,100000,2100000)]
        result=summarize(rows,25000,1100000)
        self.assertEqual(result['ticks_started'],2)
        self.assertEqual(result['ticks_overlapping'],2)
        self.assertEqual(result['over_50ms'],1)
        self.assertEqual(result['max_ms'],2000)
        self.assertEqual(result['p99_ms'],2000)  # no 1000-ms clipping
        self.assertEqual(result['boundary_ticks'],1)
        self.assertAlmostEqual(result['tps']*result['duration_s'],2)

    def test_tick_spanning_whole_phase_is_visible(self):
        result=summarize([self.row(0,0,2000000)],500000,1500000)
        self.assertEqual(result['ticks_started'],0)
        self.assertEqual(result['tps'],0)
        self.assertEqual(result['ticks_overlapping'],1)
        self.assertEqual(result['p50_ms'],2000)
        self.assertEqual(quantile([], .95),None)

    def test_fast_tail_after_phase_is_not_discarded(self):
        first=self.row(0,0,10000);late=self.row(1,2000000,2010000)
        late['fast_us']=1900000
        result=summarize([first,late],100000,1000000)
        self.assertEqual(result['ticks_started'],0)
        self.assertEqual(result['busy_max_ms'],1900)
        self.assertEqual(result['busy_over_50ms'],1)
        self.assertEqual(result['busy_boundary_intervals'],1)
        self.assertAlmostEqual(result['covered_fast_s'],1.9)

    def test_zombie_process_has_no_memory_metrics(self):
        proc=subprocess.Popen(['/bin/true'])
        try:
            os.waitid(os.P_PID,proc.pid,os.WEXITED|os.WNOWAIT)
            with self.assertRaises(ProcessLookupError):proc_sample(proc.pid)
        finally:proc.wait()

    def test_trace_detects_missing_ticks(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'ticks.csv'
            with path.open('w') as f:
                writer=csv.DictWriter(f,fieldnames=['idx','start_us','end_us']);writer.writeheader()
                writer.writerows([dict(idx=0,start_us=0,end_us=10),dict(idx=2,start_us=20,end_us=30)])
            with self.assertRaises(AssertionError):read_trace(path)

    def test_fsync_errors_and_crossing_calls(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'fsync.csv'
            path.write_text('0,1000,4,12,0,/world/players/data\n2000,5000,8,12,-1,/world/file\n')
            result=io_summary(path,1500,4000)
            self.assertEqual(result['calls'],1)
            self.assertEqual(result['errors'],1)
            self.assertAlmostEqual(result['wall_s'],.003)
            self.assertEqual(result['max_ms'],3)


class ActionAcknowledgementTest(unittest.IsolatedAsyncioTestCase):
    def bot(self):
        return ProfileBot(0,SimpleNamespace(run_id='ACK',mode='blocks',timeout=.05),lambda _:None)

    async def test_state_alone_does_not_confirm_action(self):
        b=self.bot();b.sequence=7;b.block_events[(0,64,0)].put_nowait(1)
        self.assertIsNone(await b.expect_block((0,64,0),lambda state:state==1))
        self.assertFalse(b.acknowledged)

    async def test_matching_ack_completes_state_transition(self):
        b=self.bot();b.sequence=7;b.block_events[(0,64,0)].put_nowait(1)
        job=asyncio.create_task(b.expect_block((0,64,0),lambda state:state==1))
        await b.play_packet(4,varint(6));await asyncio.sleep(.001)
        self.assertFalse(job.done())
        await b.play_packet(4,varint(7));self.assertEqual(await job,1)
        self.assertEqual(b.acknowledged[0]['sequence'],7)
        with self.assertRaises(AssertionError):await b.play_packet(4,varint(8)+b'\0')


if __name__=='__main__':unittest.main()
