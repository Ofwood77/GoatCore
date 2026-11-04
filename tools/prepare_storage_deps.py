#!/usr/bin/env python3
"""Pinned, private build SDK. No installation and no production service changes."""
import hashlib
import io
from pathlib import Path
import urllib.request
import zipfile
ROOT=Path(__file__).resolve().parents[1]
DEST=ROOT/'.local/storage-deps'
ZIP_SHA='acb1e6f5d832484bf6d32b681e858c38add8b2acdfd42ac5df24b8afb46552b4'
C_SHA3='32d5424f97e0a7fc5ed2f6335afbb58be4e0298bd7117a34e39d345ff13d859e'
HEADERS={
'lz4.h':('https://raw.githubusercontent.com/lz4/lz4/v1.9.4/lib/lz4.h','c1614ecf7ada7b0be1acb560d4239595f96fbb7aa6a79a7c40cb358753830be6'),
'zstd.h':('https://raw.githubusercontent.com/facebook/zstd/v1.5.4/lib/zstd.h','68fe7e0c59f4ceee1e05d09c16107f37d0185a8d412b8a1b4aa9f06ce3ba7839')}
def fetch(url):
 with urllib.request.urlopen(url,timeout=60) as response:
  b=response.read(16*1024*1024+1)
  if len(b)>16*1024*1024:raise ValueError('SDK download exceeds bound')
  return b

def main():
 DEST.mkdir(parents=True,exist_ok=True);(DEST/'include').mkdir(exist_ok=True)
 cached=DEST/'sqlite-source.zip'
 b=cached.read_bytes() if cached.exists() else fetch('https://www.sqlite.org/2026/sqlite-amalgamation-3510300.zip')
 assert hashlib.sha256(b).hexdigest()==ZIP_SHA,'SQLite archive hash mismatch'
 if not cached.exists():cached.write_bytes(b)
 archive=zipfile.ZipFile(io.BytesIO(b))
 for name,path in [('sqlite3.c',DEST/'sqlite3.c'),('sqlite3.h',DEST/'include/sqlite3.h')]:
  content=archive.read('sqlite-amalgamation-3510300/'+name)
  if name=='sqlite3.c':assert hashlib.sha3_256(content).hexdigest()==C_SHA3
  if not path.exists() or path.read_bytes()!=content:path.write_bytes(content)
 for name,(url,digest) in HEADERS.items():
  path=DEST/'include'/name
  content=path.read_bytes() if path.exists() else fetch(url)
  assert hashlib.sha256(content).hexdigest()==digest,name+' hash mismatch'
  if not path.exists():path.write_bytes(content)
 print('storage SDK: SQLite 3.51.3 official SHA3 verified; private LZ4/Zstd headers')
if __name__=='__main__':main()
