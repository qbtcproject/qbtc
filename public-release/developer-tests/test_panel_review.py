"""Regression checks for slow startup, cancellable pinned downloads, empty state and exact payment retry.
Linux PowerShell with local fixtures; not native Windows GUI verification.
"""
import hashlib,http.server,json,os,pathlib,shutil,subprocess,tempfile,threading
ROOT=pathlib.Path(__file__).resolve().parents[1]
PWSH=os.environ.get('QBTC_PWSH','/tmp/qbtc-pwsh/pwsh')
with tempfile.TemporaryDirectory(prefix='qbtc-panel-review-') as tmp:
 t=pathlib.Path(tmp);bundle=t/'bundle';bundle.mkdir();(bundle/'bin').mkdir()
 for n in ['TOR-PROCESS.ps1','PANEL.ps1','PANEL-LAYOUT.ps1','WALLET-ACTION.ps1']:shutil.copy2(ROOT/n,bundle/n)
 payload=b'pinned local fixture'*10000
 class Handler(http.server.BaseHTTPRequestHandler):
  def do_GET(self):
   if self.path=='/missing':self.send_error(404);return
   self.send_response(200);self.end_headers();self.wfile.write(payload)
  def log_message(self,*args):pass
 server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
 threading.Thread(target=server.serve_forever,daemon=True).start()
 helper=bundle/'TOR-PROCESS.ps1';s=helper.read_text();s=s.replace('https://dist.torproject.org/torbrowser/15.0.25/tor-expert-bundle-windows-x86_64-15.0.25.tar.gz',f'http://127.0.0.1:{server.server_port}/missing').replace('https://archive.torproject.org/tor-package-archive/torbrowser/15.0.25/tor-expert-bundle-windows-x86_64-15.0.25.tar.gz',f'http://127.0.0.1:{server.server_port}/fixture');helper.write_text(s)
 script=t/'test.ps1';script.write_text(r'''
$ErrorActionPreference='Stop'
. (Join-Path $env:FIXTURE 'bundle/TOR-PROCESS.ps1')
function Check-Cancel {if($script:cancel){throw 'Panel stop requested.'}}
function Write-State($phase,$message){$script:lastPhase=$phase}
function Run-Checked { $script:tries++; if($script:tries -lt 4){throw 'Not ready'}; return 'network=mainnet height=0' }
$node=[pscustomobject]@{HasExited=$false};$node | Add-Member ScriptMethod Refresh {}
Wait-LocalNode $node 'unused' 'unused' 3
if($script:tries -ne 4){throw 'Slow node startup not retried'}
$script:cancel=$true
try{Wait-LocalNode $node 'unused' 'unused' 3;throw 'Cancellation swallowed'}catch{if($_.Exception.Message -ne 'Panel stop requested.'){throw}}
$script:cancel=$false;$node.HasExited=$true
try{Wait-LocalNode $node 'unused' 'unused' 3;throw 'Dead node accepted'}catch{if($_.Exception.Message -notlike 'Node stopped during startup*'){throw}}
$node.HasExited=$false
function Run-Checked {throw 'Never ready'}
try{Wait-LocalNode $node 'unused' 'unused' 1;throw 'Timeout not enforced'}catch{if($_.Exception.Message -notlike '*within 1 seconds*'){throw}}
$archive=Join-Path $env:FIXTURE 'download'
$ProgressPreference='Continue'
Get-PinnedTorArchive $archive $env:FIXTURE_HASH
if($ProgressPreference -ne 'Continue'){throw 'Progress preference not restored'}
try{Get-PinnedTorArchive $archive ('0'*64);throw 'Checksum ignored'}catch{if($_.Exception.Message -notlike '*checksum mismatch*'){throw}}
$log=Join-Path $env:FIXTURE 'node.log'
[IO.File]::WriteAllText($log,'previous failure');Rotate-NodeLog $log
if((Get-Content ($log+'.previous') -Raw) -ne 'previous failure'){throw 'Previous log lost'}
# Execute the real address-read function against an empty persisted file.
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $env:FIXTURE 'bundle/PANEL.ps1'),[ref]$tokens,[ref]$errors)
$definition=$ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Read-Address'},$true)
. ([scriptblock]::Create($definition.Extent.Text))
$data=$env:FIXTURE;$script:address='';$receive=[pscustomobject]@{Text=''};$create=[pscustomobject]@{Text=''}
[IO.File]::WriteAllText((Join-Path $data 'address.txt'),'');Read-Address
if($script:address){throw 'Empty address accepted'}
$layout=Get-Content (Join-Path $env:FIXTURE 'bundle/PANEL-LAYOUT.ps1')
$onionPath=Join-Path $data 'onion-address.txt';[IO.File]::WriteAllText($onionPath,'')
$ownOnionBox=[pscustomobject]@{Text=''}
. ([scriptblock]::Create(($layout | Where-Object {$_.StartsWith('if(Test-Path -LiteralPath $onionPath)')})))
if($ownOnionBox.Text){throw 'Empty onion accepted'}
$modePath=Join-Path $data 'mining-mode.txt';[IO.File]::WriteAllText($modePath,'');$initialMode='medium'
. ([scriptblock]::Create(($layout | Where-Object {$_.StartsWith('if(Test-Path -LiteralPath $modePath)')})))
if($initialMode -ne 'medium'){throw 'Empty mining mode lost default'}
'PASS slow node retries, cancellation, exited process, deadline, official-source fallback with exact hash, preference restored, log rotation and empty address/onion/mining-mode'
''')
 env=dict(os.environ,FIXTURE=str(t),FIXTURE_HASH=hashlib.sha256(payload).hexdigest(),LOCALAPPDATA=str(t/'local'))
 try:subprocess.run([PWSH,'-NoProfile','-File',script],env=env,check=True,timeout=20)
 finally:server.shutdown();server.server_close()
 # Run actual wallet-action coordinator with deterministic fake signer and submitter.
 signer=bundle/'bin/qbtc-wallet.exe';signer.write_text('''#!/usr/bin/env python3
import sys,pathlib,os
p=pathlib.Path(os.environ['FIXTURE'])
if sys.argv[1]=='payment-status':
 print('confirmed height=123 txid='+'a'*128 if (p/'confirmed').exists() else 'unconfirmed');sys.exit(0)
c=p/'sign-count';c.write_text(str(int(c.read_text())+1) if c.exists() else '1')
if (p/'cancel-signing').exists():sys.exit(1)
pathlib.Path(sys.argv[7]).write_bytes(b'exact signed transaction fixture')
''');signer.chmod(0o755)
 peer=bundle/'bin/qbtc-peer.exe';peer.write_text('''#!/usr/bin/env python3
import sys,pathlib,os,hashlib
p=pathlib.Path(os.environ['FIXTURE']);c=p/'submissions';data=pathlib.Path(sys.argv[3]).read_bytes()
with c.open('a') as f:f.write(hashlib.sha256(data).hexdigest()+'\\n')
assert sys.argv[2]=='127.0.0.1:19021'
if len(c.read_text().splitlines())==1:sys.exit(1)
print('accepted')
''');peer.chmod(0o755)
 def action(a):
  req=t/'request.json';req.write_text(json.dumps(dict(action=a,address='qbc1fixture',amount='1',fee='0',endpoint='untrusted:123')))
  subprocess.run([PWSH,'-NoProfile','-File',bundle/'WALLET-ACTION.ps1','-RequestFile',req],env=env,input='\n',text=True,stdout=subprocess.DEVNULL,check=True,timeout=15)
  return json.loads(pathlib.Path(str(req)+'.result').read_text(encoding='utf-8-sig'))
 first=action('send');assert not first['ok'];assert 'Submission is unresolved' in first['text'],first
 pending=t/'local/QBTC-test-v2/pending-payment.json';assert pending.exists()
 assert not action('send')['ok'] and (t/'sign-count').read_text()=='1'
 record=json.loads(pending.read_text());tx=pending.parent/record['file'];original=tx.read_bytes();tx.write_bytes(b'tampered')
 assert not action('retry')['ok'] and len((t/'submissions').read_text().splitlines())==1
 tx.write_bytes(original);assert action('retry')['ok'] and pending.exists()
 assert not action('send')['ok'] and (t/'sign-count').read_text()=='1'
 (t/'confirmed').touch();assert action('retry')['ok'] and not pending.exists() and not tx.exists()
 assert (t/'sign-count').read_text()=='1'
 assert len(set((t/'submissions').read_text().splitlines()))==1
 (t/'cancel-signing').touch();assert not action('send')['ok'] and not pending.exists()
 print('PASS ambiguous submission retained; duplicate signing blocked; modified transaction rejected; retry submits identical bytes without signing; confirmation reconciles and removes only the saved confirmed transaction; cancellation clears unsigned reservation')
