"""Package the isolated CPU measurement tool; no chain, wallet or peer settings included."""
from pathlib import Path
import hashlib
import shutil
import tempfile
import zipfile
import time

root=Path(__file__).resolve().parents[1];source=root/'public-release'
target=root/'dist/downloads/QBTC-CPU-Calibration-Windows.zip'
with tempfile.TemporaryDirectory(prefix='qbtc-calibration-package-') as temporary:
    package=Path(temporary)/'QBTC-CPU-Calibration';app=package/'app'
    (app/'bin').mkdir(parents=True)
    shutil.copy2(source/'launcher/QBTC-Calibrate.exe',package/'QBTC-Calibrate.exe')
    shutil.copy2(source/'CALIBRATE.ps1',app/'CALIBRATE.ps1')
    shutil.copy2(source/'bin/qbtc-peer.exe',app/'bin/qbtc-peer.exe')
    shutil.copytree(source/'licenses',app/'licenses')
    for name in ('LICENSE','NOTICE'):shutil.copy2(source/name,app/name)
    (package/'README.txt').write_text('''QBTC CPU CALIBRATION - NO COINS ARE MINED

1. Stop QBTC mining on this PC and close other CPU-heavy applications.
2. Extract the full ZIP and open QBTC-Calibrate.exe. Keep the app folder beside it.
3. Click Measure CPU (30 seconds). Wait, then click Copy result.
4. Paste that result into the QBTC conversation. Repeat on the other PC.

This runs an isolated benchmark in the exact miner binary shipped with the current
experimental desktop. It uses High mode: all CPU threads except one, at least one.
It does not open a chain or wallet, contact peers, or generate spendable coins.
The CPU benchmark can be closed at any time. It does not change mining difficulty.
Reports contain timing, hash counts, worker count and binary identity, without
passwords, signing keys or wallet addresses. Results are saved under:
%LOCALAPPDATA%\\QBTC-calibration-v1

Measurements are optional diagnostic evidence; this tool never selects difficulty.
The final plan separately reserves a combined 2.026 MH/s design reference for a
600-second network-wide target, with adjustment every 2016 blocks. It is not an
activated network or ten minutes per individual PC. More total power changes spacing.
Experimental balances do not automatically transfer to the future official chain.

Unsigned Windows x64 software. Cross-built; native Windows execution still pending.
Source and limitations: https://qbtcproject.com
''')
    manifest=app/'SHA256SUMS.txt'
    manifest.write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+p.relative_to(package).as_posix()+'\n'
        for p in sorted(package.rglob('*')) if p.is_file() and p!=manifest))
    with zipfile.ZipFile(target,'w',zipfile.ZIP_DEFLATED) as z:
        for p in sorted(package.rglob('*')):
            if p.is_file():
                entry=zipfile.ZipInfo(package.name+'/'+p.relative_to(package).as_posix(),time.gmtime(1792627200)[:6])
                entry.create_system=3;entry.external_attr=(0o100755 if p.suffix=='.exe' else 0o100644)<<16
                entry.compress_type=zipfile.ZIP_DEFLATED
                z.writestr(entry,p.read_bytes(),compress_type=zipfile.ZIP_DEFLATED,compresslevel=9)
sha=hashlib.sha256(target.read_bytes()).hexdigest()
manifest=root/'dist/downloads/SHA256SUMS.txt'
lines=[line for line in manifest.read_text().splitlines() if not line.endswith(target.name)]
manifest.write_text('\n'.join(lines+[sha+'  '+target.name])+'\n')
print(target.name,target.stat().st_size,sha)
