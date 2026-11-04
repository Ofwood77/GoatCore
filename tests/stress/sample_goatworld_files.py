#!/usr/bin/env python3
"""Bounded external stat-only sampler; never opens a SQLite database descriptor."""
import argparse
import json
from pathlib import Path
import time

def run(a):
 start=time.monotonic()
 with a.output.open('x') as out:
  while time.monotonic()-start<a.seconds:
   for directory in sorted(a.results.glob('sqlite-*/runtime/world/goatworld')):
    files={}
    for name in ('chunks.sqlite','chunks.sqlite-wal','chunks.sqlite-shm'):
     try:
      st=(directory/name).stat();files[name]=dict(bytes=st.st_size,allocated_bytes=st.st_blocks*512)
     except FileNotFoundError:pass
    out.write(json.dumps(dict(time_us=time.monotonic_ns()//1000,run=directory.parents[2].name,files=files))+'\n')
   out.flush()
   try:
    if len(json.loads((a.results/'comparison.json').read_text()))>=a.expected:break
   except (FileNotFoundError,json.JSONDecodeError):pass
   time.sleep(.25)
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--results',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--expected',type=int,default=36);p.add_argument('--seconds',type=int,default=1800)
 a=p.parse_args();assert 1<=a.seconds<=3600;run(a)
