"""Test/source packaging helper. Use package-launch-candidate.py for the complete published set."""
from pathlib import Path
import hashlib
import json
import re
import shutil
import tempfile
import zipfile
import time

PACKAGE_VERSION = "0.9.0-preview.5"
ARCHIVE_EPOCH = 1792627200  # Fixed packaging timestamp; never a chain timestamp.

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'public-release'
DOWNLOADS = ROOT / 'dist/downloads'
DESKTOP = 'QBTC-Test-Windows.zip'
SOURCES = 'QBTC-Source.zip'
RUNTIME = [
    'START.ps1', 'PANEL.ps1', 'PANEL-LAYOUT.ps1', 'PANEL-TOR.ps1', 'TOR-PROCESS.ps1',
    'WALLET-ACTION.ps1', 'OnionBridge.cs', 'OnionNetwork.cs', 'PanelOwnedJob.cs',
    'ONION-SEEDS.txt', 'LICENSE', 'NOTICE', 'MAINNET-GENESIS.json',
    'bin/qbtc.exe', 'bin/qbtc-peer.exe', 'bin/qbtc-wallet.exe',
]
README = '''QBTC - WINDOWS 10/11 x64
Version 0.9.0-preview.5 | PRIVATE TEST NETWORK
This is not the official network. Experimental balances will not automatically carry over.

1. Extract the complete ZIP to a folder on your computer.
2. Open QBTC.exe. Keep the app folder beside it.
3. Create or unlock your wallet. Keep an encrypted backup and its password.
4. Choose Low, Medium or High, then Start mining. Start node runs without mining.

Connection through Tor is automatic. The optional extra-peer field can stay empty.
First start requires Internet access to download the pinned Tor runtime.
Low uses one CPU thread; Medium uses half; High uses all except one.
You can change power while mining. Use Stop node / mining before closing.
Refresh balance and the eye button do not request the wallet password.
Signing payments and opening the signing key still require the password.
The native signer shows destination/amount/fee and asks you to type SEND first.
If submission fails, use Retry signed payment. It resends the same saved transaction
without signing again. New payments are blocked while its submission is unresolved.
The cached Tor archive is verified and its runtime refreshed on every start.
A missing archive is downloaded once; your onion identity and wallet are retained.
On this update, unlock your wallet once to bind its address to the wallet file.
After a submitted payment, wait for confirmation before signing another.
Verify backup checks a backup without replacing your active wallet.
Restore backup requires an empty wallet slot and never overwrites an active wallet.

Updating: stop and close the old panel, then extract this ZIP into a new folder.
Security hardening: stop and close both test panels before updating.
Existing test-v2 balances and Tor identity are preserved. Legacy v1-only data
are archived on first migration; no legacy v1 balances are imported.
Existing test-v2 wallet and chain are reused from:
%LOCALAPPDATA%\\QBTC-test-v2

Experimental, unsigned software. New release changes still need native Windows verification.
Package version and file identities are recorded in app/release.json.
Oversized payments are refused before signing; use a smaller payment if requested.
Mining rewards mature after a 100-block height difference. Peer availability varies.
The app folder contains required components and license notices; do not move it.
If startup fails, a dialog reports the error. Details: panel-startup.log in the data folder.
Source code and developer tests are a separate download at https://qbtcproject.com
'''

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def manifest(folder, target):
    target.write_text(''.join(digest(f) + '  ' + f.relative_to(folder).as_posix() + '\n'
        for f in sorted(folder.rglob('*')) if f.is_file() and f != target))

def archive(folder, target):
    with zipfile.ZipFile(target, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in sorted(folder.rglob('*')):
            if f.is_file():
                entry = zipfile.ZipInfo(folder.name + '/' + f.relative_to(folder).as_posix(), time.gmtime(ARCHIVE_EPOCH)[:6])
                entry.create_system = 3
                entry.external_attr = (0o100755 if f.suffix == '.exe' else 0o100644) << 16
                entry.compress_type = zipfile.ZIP_DEFLATED
                z.writestr(entry, f.read_bytes(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)

DOWNLOADS.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='qbtc-public-package-') as temporary:
    stage = Path(temporary)
    desktop = stage / 'QBTC'
    app = desktop / 'app'
    for name in RUNTIME:
        destination = app / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(SOURCE / name, destination)
    shutil.copytree(SOURCE / 'licenses', app / 'licenses')
    shutil.copy2(SOURCE / 'launcher/QBTC.exe', desktop / 'QBTC.exe')
    (desktop / 'README.txt').write_text(README)
    release = {
        'schema': 'qbtc-release-v1', 'version': PACKAGE_VERSION, 'channel': 'private-test',
        'official_network': False, 'network_identity': json.loads((SOURCE / 'MAINNET-GENESIS.json').read_text()),
        'data_directory': '%LOCALAPPDATA%\\QBTC-test-v2',
        'native_windows_verification': 'pending_for_this_revision',
        'runtime_sha256': {name: digest(app / name) for name in RUNTIME},
        'launcher_sha256': digest(desktop / 'QBTC.exe'),
    }
    (app / 'release.json').write_text(json.dumps(release, indent=2)+'\n')
    manifest(desktop, app / 'SHA256SUMS.txt')
    archive(desktop, DOWNLOADS / DESKTOP)

    source = stage / 'QBTC-Source'
    source.mkdir()
    for f in sorted(SOURCE.rglob('*')):
        relative = f.relative_to(SOURCE)
        if any(part in ('.deps', '.git', '__pycache__') or part.startswith('build-') or part == 'build' for part in relative.parts[:-1]):
            continue
        if not f.is_file() or relative.parts[0] in ('bin','final-bin') or f.name == 'SHA256SUMS.txt' or f.suffix in ('.exe', '.o'):
            continue
        destination = source / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(f, destination)
    (source / 'DEVELOPERS.txt').write_text(
        'QBTC 0.9.0 source and developer tests. Windows binaries are a separate download.\n'
        'Native core: native-source/. Enable QBTC_MAINNET in CMake for mainnet builds.\n'
        'Pinned dependencies: native-source/DEPENDENCIES.lock.\n'
        'Desktop scripts and C# runtime components are at the top level.\n')
    shutil.copy2(ROOT / 'scripts/build-launcher.py', source / 'launcher/build-launcher.py')
    (source / 'launch-tools').mkdir()
    for name in ('launch-calibration.py', 'study-launch-emission.py', 'test-launch-calibration.py', 'test-final-launch-plan.py'):
        shutil.copy2(ROOT / 'scripts' / name, source / 'launch-tools' / name)
    manifest(source, source / 'SHA256SUMS.txt')
    archive(source, DOWNLOADS / SOURCES)

(DOWNLOADS / 'README.txt').write_text(README)
(DOWNLOADS / 'release.json').write_text(json.dumps(dict(release, desktop_file=DESKTOP, source_file=SOURCES, desktop_sha256=digest(DOWNLOADS / DESKTOP), source_sha256=digest(DOWNLOADS / SOURCES)), indent=2)+'\n')
(DOWNLOADS / 'SHA256SUMS.txt').write_text(''.join(
    digest(DOWNLOADS / name) + '  ' + name + '\n' for name in (DESKTOP, SOURCES)))
page = ROOT / 'dist/index.html'
text = page.read_text()
text = re.sub(r'(<code id="hash">)[a-f0-9]{64}', r'\g<1>' + digest(DOWNLOADS / DESKTOP), text)
page.write_text(text)
for old_name in ('QBTC-0.9.0-desktop-balance1-Windows.zip', 'QBTC-0.9.0-source-balance1.zip'):
    old = DOWNLOADS / old_name
    if old.exists():
        shutil.move(old, ROOT / 'release-archive' / old.name)
for name in (DESKTOP, SOURCES):
    print(name, (DOWNLOADS / name).stat().st_size, digest(DOWNLOADS / name))
