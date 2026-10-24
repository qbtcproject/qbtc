"""Package the frozen isolated launch candidate; never starts a node or mines rewards.
Run package-desktop.py first to retain the separately named current-test download/source.
"""
from pathlib import Path
import runpy,hashlib,json,shutil,tempfile,re,zipfile
ROOT=Path(__file__).resolve().parents[1]
# Existing packaging is also the source/manifests authority for the test download.
m=runpy.run_path(str(ROOT/'scripts/package-desktop.py'))
SOURCE=ROOT/'public-release';DOWNLOADS=ROOT/'dist/downloads'
g=json.loads((SOURCE/'FINAL-GENESIS.json').read_text())
old=json.loads((SOURCE/'MAINNET-GENESIS.json').read_text())
runtime=m['RUNTIME'];digest=m['digest']
with tempfile.TemporaryDirectory(prefix='qbtc-launch-candidate-') as temp:
 desktop=Path(temp)/'QBTC';app=desktop/'app';app.mkdir(parents=True)
 for name in runtime:
  dest=app/name;dest.parent.mkdir(parents=True,exist_ok=True)
  src=SOURCE/name
  if name.startswith('bin/'):src=SOURCE/'final-bin'/Path(name).name
  shutil.copy2(src,dest)
  if dest.suffix in ('.ps1','.cs'):
   s=dest.read_text().replace('QBTC-test-v2','QBTC-mainnet-2026').replace('tor-trial','tor')
   s=s.replace(old['genesis_hash'],g['genesis_hash'])
   s=s.replace('0x32,0x4d,0x42,0x51,1,0,0,0', '0x33,0x4d,0x42,0x51,1,0,0,0')
   s=s.replace('TEST NETWORK','LAUNCH CANDIDATE').replace('Experimental network. Official QBTC launch is pending.','Frozen launch candidate. Public launch verification remains pending.')
   if name=='START.ps1':
    a=s.index("    $old=Join-Path");b=s.index('    $null=New-Item -ItemType Directory -Path $folder',a)
    # Do not migrate ANY test chain, identity or wallet into the candidate.
    # Select the final directory-creation statement after the legacy migration block.
    b=s.index("    $null=New-Item -ItemType Directory -Path $folder -Force",a)
    s=s[:a]+s[b:]
   dest.write_text(s)
 (app/'ONION-SEEDS.txt').write_text('# No test-network seeds are carried into the frozen launch candidate.\n# Bootstrap with a separately shared candidate node address in Extra peer.\n')
 (app/'MAINNET-GENESIS.json').write_text(json.dumps(g,indent=2)+'\n')
 shutil.copytree(SOURCE/'licenses',app/'licenses')
 shutil.copy2(SOURCE/'launcher/QBTC.exe',desktop/'QBTC.exe')
 readme='''QBTC 1.0.0-rc.1 - FROZEN LAUNCH CANDIDATE | WINDOWS 10/11 x64

This is a separate, new network candidate, not the test-v2 chain.
No test balances, wallet files, chain history or onion identities are imported.
Genesis: fuckgreed. Zero genesis allocation. Reference: 2,026,000 H/s total.
Expected block time at that reference: 600 seconds. Retarget: every 2016 blocks.
Real intervals are random and depend on total active hashpower.

1. Close any other QBTC panel and node; these profiles use the same local ports.
2. Extract the entire ZIP and open QBTC.exe inside the QBTC folder.
3. Create a NEW wallet, save its encrypted backup and verify the backup.
4. Start node to obtain Your node address. Keep this panel open.
5. The first participant shares this candidate onion address with the next participant.
6. On the second PC, paste it in Extra peer, save, then start the node.
7. Compare chain tip/hash and peer reachability before mining together.
8. Choose Low and Start mining. First-node mining is possible without peers;
   without a shared contact, separate nodes can create separate valid forks.

No test seeds are embedded. Distinct onion addresses do not prove independent owners.
Multiple synchronization lanes and bounded discovery reduce single-source dependence;
they do not provide complete Sybil/eclipse or denial-of-service immunity.

Data: %LOCALAPPDATA%\\QBTC-mainnet-2026
The current test download uses %LOCALAPPDATA%\\QBTC-test-v2 separately.
Never copy chain.db or wallet.qwlt between these networks.

Unsigned candidate. Native Windows 5.1, Tor and unrelated-network testing plus
independent security review remain pending. This package alone does not mean
that a public network has been launched. Public website access remains private.
'''
 (desktop/'README.txt').write_text(readme)
 release=dict(schema='qbtc-release-v1',version='1.0.0-rc.1',channel='private-launch-candidate',official_network=False,operational_network_launched=False,network_identity=g,data_directory='%LOCALAPPDATA%\\QBTC-mainnet-2026',native_windows_verification='pending_for_this_revision',runtime_sha256={n:digest(app/n) for n in runtime},launcher_sha256=digest(desktop/'QBTC.exe'))
 (app/'release.json').write_text(json.dumps(release,indent=2)+'\n')
 m['manifest'](desktop,app/'SHA256SUMS.txt');m['archive'](desktop,DOWNLOADS/'QBTC-Windows.zip')
 (DOWNLOADS/'README.txt').write_text(readme)
 (DOWNLOADS/'release.json').write_text(json.dumps(dict(release,desktop_file='QBTC-Windows.zip',source_file='QBTC-Source.zip',desktop_sha256=digest(DOWNLOADS/'QBTC-Windows.zip'),source_sha256=digest(DOWNLOADS/'QBTC-Source.zip')),indent=2)+'\n')
 names=['QBTC-Windows.zip','QBTC-Test-Windows.zip','QBTC-Source.zip']
 (DOWNLOADS/'SHA256SUMS.txt').write_text(''.join(digest(DOWNLOADS/n)+'  '+n+'\n' for n in names))
 page=ROOT/'dist/index.html';s=page.read_text();s=re.sub(r'(<code id="hash">)[a-f0-9]{64}',r'\g<1>'+digest(DOWNLOADS/'QBTC-Windows.zip'),s);page.write_text(s)
 print('QBTC-Windows.zip',digest(DOWNLOADS/'QBTC-Windows.zip'))
