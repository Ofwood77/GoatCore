#!/usr/bin/env python3
"""G2 transaction, recovery and corruption tests on disposable worlds only."""
import argparse
from contextlib import closing
import json
import os
from pathlib import Path
import select
import shutil
import sqlite3
import subprocess
import tempfile
import unittest
TOOL=FAULT=PROBE=None
EVENTS=[]
class Integrated(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory(prefix='goatcore-g2-crash-');self.base=Path(self.tmp.name)
  self.original=self.base/'original';self.original.mkdir();(self.original/'.goatcore-g2-disposable').write_text('test\n')
  self.call('seed',self.original)
 def tearDown(self):self.tmp.cleanup()
 def call(self,mode,world,success=True):
  p=subprocess.run([str(TOOL),'--disposable',mode,str(world)],capture_output=True,text=True,timeout=30)
  self.assertNotRegex(p.stderr,r'AddressSanitizer|LeakSanitizer|ThreadSanitizer|runtime error:')
  if success:self.assertEqual(p.returncode,0,p.stderr);return json.loads(p.stdout.splitlines()[-1])
  self.assertNotEqual(p.returncode,0);return p
 def copy(self,name):
  path=self.base/name;shutil.copytree(self.original,path);return path
 def fault(self,world,phase,error=None,operation='update'):
  r,w=os.pipe();a,b=os.pipe()
  env=dict(os.environ,G1_TEST_ROOT=str(world/'goatworld'),G1_TEST_PHASE=phase,G1_TEST_READY=str(w),G1_TEST_RELEASE=str(a),LD_PRELOAD=(os.environ.get('G1_SANITIZER_PRELOAD','')+':'+str(PROBE)).lstrip(':'))
  if error:env['G1_TEST_ERROR']=error
  p=subprocess.Popen([str(FAULT),'--disposable',operation,str(world)],env=env,pass_fds=(w,a),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  os.close(w);os.close(a)
  try:
   self.assertTrue(select.select([r],[],[],30)[0],phase)
   self.assertEqual(os.read(r,1),b'R',(phase,p.poll()))
   if error:os.write(b,b'G')
   else:p.kill()
   stdout,stderr=p.communicate(timeout=30)
   self.assertNotRegex(stderr.decode(),r'AddressSanitizer|LeakSanitizer|ThreadSanitizer|runtime error:')
   return p.returncode,stdout.decode(),stderr.decode()
  finally:
   if p.poll() is None:p.kill();p.communicate(timeout=10)
   os.close(r);os.close(b)
 def test_crashes(self):
  for phase in ('prepare','sql-begin','sql-inserted','sql-before-commit','sql-committed','durable','checkpoint-before','checkpoint-after','close-before','close-after'):
   with self.subTest(phase=phase):
    world=self.copy(phase);code,_,_=self.fault(world,phase);self.assertEqual(code,-9)
    first=self.call('read',world);second=self.call('read',world);self.assertEqual(first,second)
    old=phase in ('prepare','sql-begin','sql-inserted','sql-before-commit')
    self.assertEqual(first['states'],[1,1] if old else [2,2]);self.assertEqual(first['generations'],[1,1] if old else [2,2])
    with closing(sqlite3.connect(world/'goatworld/chunks.sqlite')) as db:self.assertEqual(db.execute('PRAGMA integrity_check').fetchone(),('ok',))
    EVENTS.append(dict(phase=phase,code=code,restored=first,idempotent=True))
 def test_io_errors_short_and_eintr(self):
  for error in ('pwrite-EIO','pwrite-ENOSPC','fsync-EIO','fsync-ENOSPC','pwrite-short','pwrite-EINTR','fsync-EINTR'):
   world=self.copy(error);code,_,stderr=self.fault(world,'prepare',error=error)
   result=self.call('read',world);self.assertIn(result['states'],([1,1],[2,2]))
   if code==0:self.assertEqual(result['states'],[2,2])
   if error.endswith(('EIO','ENOSPC')):self.assertNotEqual(code,0,stderr)
   self.assertEqual(result,self.call('read',world))
   EVENTS.append(dict(error=error,code=code,restored=result))
 def test_sqlite_internal_io_crashes(self):
  for phase in ('sqlite-wal-partial','sqlite-wal-sync-before','sqlite-checkpoint-partial'):
   world=self.copy(phase);code,_,_=self.fault(world,phase);self.assertEqual(code,-9)
   restored=self.call('read',world);self.assertEqual(restored,self.call('read',world))
   allowed=([1,1],) if phase=='sqlite-wal-partial' else ([2,2],) if phase=='sqlite-checkpoint-partial' else ([1,1],[2,2])
   self.assertIn(restored['states'],allowed);self.assertEqual(restored['generations'],restored['states'])
   EVENTS.append(dict(phase=phase,code=code,restored=restored,idempotent=True,inside_sqlite_io=True))
 def test_corruption_and_versions(self):
  for name,query in [('checksum','UPDATE chunks SET sha=zeroblob(32)'),('codec','UPDATE chunks SET c=99'),('wide-codec','UPDATE chunks SET c=4294967298'),('wide-layout','UPDATE chunks SET f=4294967296'),('wide-level','UPDATE chunks SET l=4294967296'),('generation','UPDATE chunks SET g=g+1'),('typed-generation',"UPDATE chunks SET g='1a'"),('payload',"UPDATE chunks SET payload=x'01'"),('schema','PRAGMA user_version=99'),('identity','UPDATE meta SET world_id=zeroblob(16)'),('data-version','UPDATE meta SET data_version=4785')]:
   world=self.copy(name)
   with closing(sqlite3.connect(world/'goatworld/chunks.sqlite')) as db, db:
    if name=='data-version':db.execute('PRAGMA ignore_check_constraints=ON')
    db.execute(query)
   self.call('read',world,success=False)
  world=self.copy('truncated');path=world/'goatworld/chunks.sqlite';path.write_bytes(path.read_bytes()[:5000]);self.call('read',world,success=False)
  world=self.copy('missing');(world/'goatworld/chunks.sqlite').unlink();self.call('read',world,success=False);self.assertFalse((world/'goatworld/chunks.sqlite').exists())
 def test_busy_without_regeneration(self):
  world=self.copy('busy')
  db=sqlite3.connect(world/'goatworld/chunks.sqlite');db.execute('BEGIN IMMEDIATE')
  try:self.call('update',world,success=False)
  finally:db.rollback();db.close()
  self.assertEqual(self.call('read',world)['states'],[1,1])
 def test_pinned_reader_checkpoint(self):
  world=self.copy('pinned-reader')
  db=sqlite3.connect((world/'goatworld/chunks.sqlite').resolve().as_uri()+'?mode=ro',uri=True)
  try:
   db.execute('BEGIN');self.assertEqual(db.execute('SELECT g FROM chunks ORDER BY x').fetchall(),[(1,),(1,)])
   failure=self.call('update',world,success=False)
   self.assertIn('checkpoint failed rc=5',failure.stderr)
   self.assertEqual(db.execute('SELECT g FROM chunks ORDER BY x').fetchall(),[(1,),(1,)])
  finally:db.rollback();db.close()
  restored=self.call('read',world);self.assertEqual(restored,dict(generations=[2,2],states=[2,2]))
  EVENTS.append(dict(pinned_reader=True,close_reported_busy=True,committed_state=restored))
 def test_s4_error_reaches_shutdown(self):
  world=self.copy('s4-error');code,stdout,stderr=self.fault(world,'prepare',error='fsync-EIO',operation='world-update')
  self.assertNotEqual(code,0);self.assertIn('world shutdown persistence FAILED',stderr)
  self.assertIn(self.call('read',world)['states'],([1,1],[2,2]))
 def test_checkpoint_failure_reported(self):
  world=self.copy('checkpoint-error');code,_,_=self.fault(world,'checkpoint-before',error='fsync-EIO');self.assertNotEqual(code,0)
  self.assertEqual(self.call('read',world)['states'],[2,2])
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--tool',required=True,type=Path);p.add_argument('--fault-tool',required=True,type=Path);p.add_argument('--evidence',type=Path)
 a=p.parse_args();TOOL=a.tool.resolve();FAULT=a.fault_tool.resolve()
 with tempfile.TemporaryDirectory(prefix='goatcore-g2-probe-') as tmp:
  PROBE=Path(tmp)/'fault.so';subprocess.run(['cc','-std=c11','-shared','-fPIC','-O2','-o',str(PROBE),str(Path(__file__).parent/'stress/goatworld_fault.c')],check=True)
  result=unittest.main(argv=[__file__],exit=False)
 if a.evidence:a.evidence.write_text(json.dumps(dict(status='passed' if result.result.wasSuccessful() else 'failed',tests=result.result.testsRun,events=EVENTS),indent=2)+'\n')
 raise SystemExit(0 if result.result.wasSuccessful() else 1)
