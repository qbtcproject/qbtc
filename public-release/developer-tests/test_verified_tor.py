"""Verify runtime refresh/rejection using local fixture archives; never download or run Tor."""
import pathlib,tempfile,tarfile,hashlib,subprocess,os
root=pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as tmp:
 t=pathlib.Path(tmp);system=t/'system/System32';system.mkdir(parents=True);(system/'tar.exe').symlink_to('/usr/bin/tar')
 content=t/'fixture';content.mkdir();(content/'tor.exe').write_text('verified fixture; never executed')
 archive=t/'good.tar.gz'
 with tarfile.open(archive,'w:gz') as a:a.add(content/'tor.exe',arcname='tor/tor.exe')
 empty=t/'empty.tar.gz'
 with tarfile.open(empty,'w:gz'):pass
 corrupt=t/'corrupt.tar.gz';corrupt.write_bytes(b'not an archive')
 script=t/'test.ps1';script.write_text(r'''
$ErrorActionPreference='Stop'
. (Join-Path $env:QBTC_SOURCE 'TOR-PROCESS.ps1')
$runtime=Join-Path $env:QBTC_FIXTURE 'runtime'
$null=New-Item -ItemType Directory -Path $runtime
[IO.File]::WriteAllText((Join-Path $runtime 'rogue.dll'),'untrusted old runtime')
$archive=Join-Path $env:QBTC_FIXTURE 'good.tar.gz'
$hash=(Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant()
$exe=Install-VerifiedTorRuntime $archive $runtime $hash
if(!(Test-Path $exe) -or (Test-Path (Join-Path $runtime 'rogue.dll'))){throw 'old runtime was reused'}
foreach($case in @(@{path=$archive;hash=('0'*64)},@{path=(Join-Path $env:QBTC_FIXTURE 'corrupt.tar.gz');hash=$null},@{path=(Join-Path $env:QBTC_FIXTURE 'empty.tar.gz');hash=$null})) {
 if(!$case.hash){$case.hash=(Get-FileHash $case.path -Algorithm SHA256).Hash.ToLowerInvariant()}
 $rejected=$false;try{$null=Install-VerifiedTorRuntime $case.path $runtime $case.hash}catch{$rejected=$true}
 if(!$rejected -or !(Test-Path $exe)){throw 'bad archive accepted or working runtime lost'}
}
if(@(Get-ChildItem $env:QBTC_FIXTURE -Directory -Filter 'runtime.new-*').Count){throw 'staging directory leaked'}
'PASS verified runtime replaces stale files; wrong hash, corrupt archive and absent executable rejected; existing runtime preserved; no Tor code executed'
''')
 env=dict(os.environ,SystemRoot=str(t/'system'),QBTC_SOURCE=str(root),QBTC_FIXTURE=str(t))
 subprocess.run(['/tmp/qbtc-pwsh/pwsh','-NoProfile','-File',script],env=env,check=True,timeout=20)
