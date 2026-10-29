"""Pure unit checks for continuous measurement boundaries, not server tests."""
import csv
import tempfile
import unittest
from pathlib import Path
from profile_tick import summarize, quantile, read_trace, io_summary


class ContinuousProfileTest(unittest.TestCase):
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


if __name__=='__main__':unittest.main()
