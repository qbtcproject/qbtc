import pathlib,tempfile,subprocess,shutil,socket,time,json,os,tarfile,hashlib
ROOT=pathlib.Path(os.environ.get('QBTC_BUNDLE',pathlib.Path(__file__).resolve().parents[1]));PWSH=os.environ.get('QBTC_PWSH','/tmp/qbtc-pwsh/pwsh');CORE=pathlib.Path(os.environ.get('QBTC_NATIVE_BUILD','/workspace/qbtc-mainnet/QBTC-0.9.0/build-mainnet'))
FAKE_TOR=r'''#!/usr/bin/env python3
import sys,socket,pathlib,threading
if '--verify-config' in sys.argv:print('Configuration valid');sys.exit(0)
p=pathlib.Path('onion-service');p.mkdir(exist_ok=True);(p/'hostname').write_text('a'*56+'.onion')
def read(s,n):
 b=b''
 while len(b)<n:
  x=s.recv(n-len(b))
  if not x:raise EOFError()
  b+=x
 return b
def handle(s):
 target=None
 try:
  assert read(s,3)==bytes([5,1,0]);s.sendall(bytes([5,0]));h=read(s,5);read(s,h[4]);assert read(s,2)==(19021).to_bytes(2,'big')
  target=socket.create_connection(('127.0.0.1',19025));s.sendall(bytes([5,0,0,1,127,0,0,1,0,0]))
  def pump(a,b):
   try:
    while True:
     x=a.recv(16384)
     if not x:break
     b.sendall(x)
   except OSError:pass
   try:b.shutdown(socket.SHUT_WR)
   except OSError:pass
  t=threading.Thread(target=pump,args=(s,target),daemon=True);t.start();pump(target,s)
 except (OSError,EOFError,AssertionError):pass
 finally:
  s.close()
  if target:target.close()
s=socket.socket();s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);s.bind(('127.0.0.1',19050));s.listen();print('Bootstrapped 100% (done): Done',flush=True)
while True:
 c,_=s.accept();threading.Thread(target=handle,args=(c,),daemon=True).start()
'''
def waitstate(state,phase,proc,seconds=20):
 end=time.monotonic()+seconds;last=None
 while time.monotonic()<end:
  try:
   last=json.loads(state.read_text(encoding='utf-8-sig'))
   if last['phase']==phase:return last
   if last['phase']=='error':raise AssertionError(last)
  except (FileNotFoundError,json.JSONDecodeError):pass
  if proc.poll() is not None:raise AssertionError(('owner ended early',proc.returncode,last))
  time.sleep(.1)
 raise AssertionError(('state timeout',phase,last))
def openport(p):
 try:
  with socket.create_connection(('127.0.0.1',p),timeout=.2):return True
 except OSError:return False
PROFILE='QBTC-mainnet-2026' if 'QBTC-mainnet-2026' in (ROOT/'PANEL-TOR.ps1').read_text() else 'QBTC-test-v2'
TOR_FOLDER='tor' if PROFILE=='QBTC-mainnet-2026' else 'tor-trial'
checks=0
with tempfile.TemporaryDirectory(prefix='qbtc-panel-backend-') as temp:
 t=pathlib.Path(temp);bundle=t/'bundle [v5]';bundle.mkdir();(bundle/'bin').mkdir();data=t/'local'/PROFILE/TOR_FOLDER;data.mkdir(parents=True)
 for name in ['PANEL-TOR.ps1','TOR-PROCESS.ps1','OnionBridge.cs','OnionNetwork.cs','ONION-SEEDS.txt','TOR-TRIAL.ps1']:
  if (ROOT/name).exists():shutil.copy2(ROOT/name,bundle/name)
 for name in ['qbtc','qbtc-peer']:(bundle/'bin'/(name+'.exe')).symlink_to(CORE/name)
 (bundle/'PanelOwnedJob.cs').write_text('using System; using System.Diagnostics; public sealed class QbtcPanelJob : IDisposable { public void Add(Process p) {} public void Dispose() {} }')
 runtime=data/'runtime-15.0.25/tor';runtime.mkdir(parents=True);tor=runtime/'tor.exe';tor.write_text(FAKE_TOR);tor.chmod(0o755)
 archive=data/'tor-expert-15.0.25.tar.gz'
 with tarfile.open(archive,'w:gz') as packed:packed.add(runtime,arcname='tor')
 archive_hash=hashlib.sha256(archive.read_bytes()).hexdigest()
 helper=bundle/'TOR-PROCESS.ps1';helper.write_text(helper.read_text().replace("Join-Path $env:SystemRoot 'System32\\tar.exe'","'/usr/bin/tar'"))
 alive=t/'owner.alive'
 mocks='''
function Get-CimInstance {param($ClassName,$Filter);return @()}
function Get-NetTCPConnection {param($State,$LocalPort,$ErrorAction);return @()}
function Get-NetIPConfiguration {return [pscustomobject]@{NetAdapter=[pscustomobject]@{Status='Up'};IPv4DefaultGateway='fixture';InterfaceIndex=1;IPv4Address=@([pscustomobject]@{IPAddress='192.168.1.10'})}}
function Get-NetIPInterface {param($InterfaceIndex,$AddressFamily);return [pscustomobject]@{InterfaceMetric=1}}
'''
 mocks+="\nfunction Get-Process {param($Id,$ErrorAction);if(Test-Path '"+str(alive)+"'){return [pscustomobject]@{StartTime=[DateTime]::new(638000000000000000,[DateTimeKind]::Utc)}}}\n"
 source=(bundle/'PANEL-TOR.ps1').read_text().replace("$ErrorActionPreference='Stop'", "$ErrorActionPreference='Stop'\n"+mocks,1).replace(' -WindowStyle Hidden','')
 source=source.replace('c1988c701de2f24f7d098d1d6d7f507b58f27740d097f29c246e0524d639b252',archive_hash)
 source=source.replace("    if (!$owner -or $owner.StartTime.ToUniversalTime().Ticks -ne $OwnerStartTicks)","    if (!$owner -or [Math]::Abs($owner.StartTime.ToUniversalTime().Ticks-$OwnerStartTicks) -gt 10000000)") # Linux clock-to-start-time conversion tolerance only
 source=source.replace("$bindAddress=($choices | Sort-Object {$_.metric} | Select-Object -First 1).ip","$bindAddress='127.0.0.1' # Linux fixture bind only")
 (bundle/'PANEL-TOR.ps1').write_text(source)
 env=os.environ.copy();env['LOCALAPPDATA']=str(t/'local')
 remoteLog=(t/'remote.log').open('w');remote=subprocess.Popen([str(CORE/'qbtc-peer'),'serve',str(t/'remote.chain'),'19025','--control-port','19125'],stdout=remoteLog,stderr=remoteLog)
 active=[]
 try:
  for mode in ['stop-button','parent-close']:
   alive.write_text('alive')
   state=t/(mode+'.json');stop=t/(mode+'.stop');owner=t/(mode+'.ps1')
   owner.write_text(f'''$ErrorActionPreference='Stop'
$psi=New-Object Diagnostics.ProcessStartInfo
$psi.FileName='{PWSH}';$psi.UseShellExecute=$false
$psi.Arguments='-NoProfile -File "{bundle}/PANEL-TOR.ps1" -StateFile "{state}" -StopFile "{stop}" -OwnerPid '+$PID+' -OwnerStartTicks '+'638000000000000000'+' -RemoteOnion '+('b'*56)+'.onion'
$p=New-Object Diagnostics.Process;$p.StartInfo=$psi;$null=$p.Start()
[Console]::WriteLine('BACKEND='+$p.Id)
$p.WaitForExit();exit $p.ExitCode
''')
   log=(t/(mode+'.out')).open('w');proc=subprocess.Popen([PWSH,'-NoProfile','-File',str(owner)],env=env,stdout=log,stderr=log);active.append(proc)
   record=waitstate(state,'peer-verified',proc);checks+=1
   assert record['bind']=='127.0.0.1' and record['mining'] is False;checks+=1
   assert openport(19021) and openport(19050) and openport(19025);checks+=1
   if mode=='stop-button':stop.write_text('stop');proc.wait(timeout=8)
   else:proc.terminate();proc.wait(timeout=5);alive.unlink()
   end=time.monotonic()+8
   while time.monotonic()<end and (any(openport(p) for p in [19021,19022,19023,19024,19026,19028,19050])):time.sleep(.1)
   assert not any(openport(p) for p in [19021,19022,19023,19024,19026,19028,19050]),'owned listeners leaked';checks+=1
   assert remote.poll() is None and openport(19025),'external peer stopped';checks+=1
   assert json.loads(state.read_text())['phase']=='stopped';checks+=1
  print(f'PASS {checks} backend lifecycle checks: real height-zero mainnet nodes, simulated SOCKS/Tor, automatic peer verification, stop button, panel-owner exit, listener cleanup, external peer survives. Linux fixtures replace Windows job/PInvoke and private binding. No blocks mined.')
 except:
  for logpath in t.glob('*.out'):print(logpath.name,logpath.read_text())
  raise
 finally:
  for p in active:
   if p.poll() is None:p.terminate();p.wait(timeout=5)
  remote.terminate();remote.wait(timeout=5)
