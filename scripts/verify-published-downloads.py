"""Verify complete download bytes in the checkout, pushed Git snapshot and optional Site archive.
Run after site-workflow packaging, before deploying its returned commit/archive.
"""
from pathlib import Path
import argparse,hashlib,subprocess,zipfile,io,tarfile
p=argparse.ArgumentParser();p.add_argument('--archive',type=Path);p.add_argument('--git-head',action='store_true');args=p.parse_args()
r=Path(__file__).resolve().parents[1]
expected={name:sha for sha,name in (line.split('  ',1) for line in (r/'dist/downloads/SHA256SUMS.txt').read_text().splitlines())}
def verify(data,name,label):
 if hashlib.sha256(data).hexdigest()!=expected[name]:raise SystemExit(f'{label}: {name} hash mismatch / incomplete artifact')
 with zipfile.ZipFile(io.BytesIO(data)) as z:
  if not z.namelist() or z.testzip() is not None:raise SystemExit(f'{label}: {name} invalid or incomplete ZIP')
for name in expected:
 verify((r/'dist/downloads'/name).read_bytes(),name,'checkout')
 if args.git_head:verify(subprocess.check_output(['git','show','HEAD:dist/downloads/'+name],cwd=r),name,'Git HEAD')
if args.archive:
 with tarfile.open(args.archive) as a:
  for name in expected:
   matches=[m for m in a.getmembers() if m.name.removeprefix('./')=='dist/downloads/'+name]
   if len(matches)!=1:raise SystemExit('Site archive missing/ambiguous download: '+name)
   verify(a.extractfile(matches[0]).read(),name,'Site archive')
print('PASS complete ZIP bytes, SHA-256 and CRC in '+', '.join(['checkout']+(['Git HEAD'] if args.git_head else [])+(['Site archive'] if args.archive else [])))
