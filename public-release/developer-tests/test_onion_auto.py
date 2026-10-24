"""Real C# manager + native mainnet node, simulated Tor and discovery peers.
No rewarded blocks are mined. Fake onion names are fixture routing keys only.
"""
import pathlib,subprocess,tempfile,socket,threading,time,json,struct,hashlib,os,sys
ROOT=pathlib.Path(__file__).resolve().parents[1]
PWSH=os.environ.get('QBTC_PWSH','/tmp/qbtc-pwsh/pwsh')
CORE=os.environ.get('QBTC_NATIVE_PEER','/workspace/qbtc-mainnet/QBTC-0.9.0/build-mainnet/qbtc-peer')
G='000006d71412362513fb97c48c0a1371b0edd3e7e6a29e91a8318f889181e5dd1255621ebcc717083a1a135c57c12b6c3885e2e0dbfac410d43089fbb49ed73c'
MAGIC='QBTC-ONION-1 mainnet '+G
names={c:c*56+'.onion' for c in 'abcdefg'}
alive={names['b'],names['c'],names['d']};bad=set();stop=threading.Event();queries=[]
def exact(c,n):
 b=b''
 while len(b)<n:
  x=c.recv(n-len(b))
  if not x:raise EOFError()
  b+=x
 return b
def line(c):
 b=b''
 while len(b)<200:
  x=exact(c,1)
  if x==b'\n':return b.decode()
  b+=x
 raise ValueError()
def pump(a,b):
 try:
  while True:
   x=a.recv(16384)
   if not x:break
   b.sendall(x)
 except OSError:pass
 try:b.shutdown(socket.SHUT_WR)
 except OSError:pass
def socks(c):
 target=None
 try:
  c.settimeout(3);assert exact(c,3)==b'\x05\x01\x00';c.sendall(b'\x05\x00');h=exact(c,5);host=exact(c,h[4]).decode();port=int.from_bytes(exact(c,2),'big');queries.append((host,port))
  if host not in alive:c.sendall(b'\x05\x04\x00\x01'+b'\x00'*6);return
  c.sendall(b'\x05\x00\x00\x01\x7f\x00\x00\x01\x00\x00')
  if port==19023:
   assert line(c)==MAGIC;line(c);assert line(c)=='0'
   contacts=[names['c']] if host==names['b'] else []
   c.sendall((MAGIC+'\n'+host+'\n'+str(len(contacts))+'\n'+'\n'.join(contacts)+ ('\n' if contacts else '')).encode());return
  assert port==19021
  if host in bad:
   exact(c,13);c.sendall(struct.pack('<IIBI',123,1,ord('S'),1)+b'x');return
  target=socket.create_connection(('127.0.0.1',19025));target.settimeout(5)
  t=threading.Thread(target=pump,args=(c,target),daemon=True);t.start();pump(target,c)
 except (OSError,EOFError,AssertionError):pass
 finally:c.close();target and target.close()
def accept(listener):
 while not stop.is_set():
  try:c,_=listener.accept()
  except OSError:return
  threading.Thread(target=socks,args=(c,),daemon=True).start()
checks=0
def check(v,label):
 global checks
 if not v:raise AssertionError(label)
 checks+=1;print('PASS',label,flush=True)
def wait(fn,limit=15):
 end=time.monotonic()+limit
 while time.monotonic()<end:
  x=fn()
  if x:return x
  time.sleep(.15)
 raise AssertionError('Timed out')
with tempfile.TemporaryDirectory() as td:
 td=pathlib.Path(td);node=subprocess.Popen([CORE,'serve',str(td/'chain.db'),'19025','--control-port','19125'],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
 listener=socket.socket();listener.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);listener.bind(('127.0.0.1',19050));listener.listen();threading.Thread(target=accept,args=(listener,),daemon=True).start()
 script=td/'run.ps1';script.write_text('''param($Root,$Store,$Seed)
$ErrorActionPreference='Stop'
Add-Type -Path @((Join-Path $Root 'OnionBridge.cs'),(Join-Path $Root 'OnionNetwork.cs'))
$m=New-Object QbtcOnionNetwork(('a'*56+'.onion'),$Store,19050,[string[]]@($Seed),'')
Write-Output 'READY'
try { while($true){$line=[Console]::ReadLine();if($line -eq 'stop' -or $null -eq $line){break};@{known=$m.KnownCount;verified=$m.VerifiedCount;current=$m.CurrentPeer;selected=@($m.SelectedPeers);error=$m.LastError} | ConvertTo-Json -Compress} } finally {$m.Dispose()}
''')
 proc=None
 def start(seed,store=None):
  p=subprocess.Popen([PWSH,'-NoProfile','-File',str(script),str(ROOT),str(store or td/'peers.txt'),seed],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,bufsize=1)
  if p.stdout.readline().strip()!='READY':raise AssertionError(p.stderr.read())
  return p
 def state():proc.stdin.write('state\n');proc.stdin.flush();return json.loads(proc.stdout.readline())
 def stop_manager():
  proc.stdin.write('stop\n');proc.stdin.flush();proc.wait(timeout=10);check(proc.returncode==0,'manager shutdown')
 def exchange(message):
  with socket.create_connection(('127.0.0.1',19023),timeout=2) as c:
   c.settimeout(2);c.sendall(message.encode());
   try:return c.recv(2048)
   except ConnectionResetError:return b''
 try:
  time.sleep(.4);proc=start(names['b'])
  wait(lambda:state()['verified']>=2);check(True,'learn and independently probe a peer from the initial contact')
  s=state();check(s['known']==2,'own address excluded and candidate bounded')
  result=subprocess.run([CORE,'status','127.0.0.1:19022','--onion-adapter'],text=True,capture_output=True,timeout=20);check(result.returncode==0 and 'network=mainnet' in result.stdout,'unchanged native mainnet protocol through automatic bridge')
  check(exchange('WRONG\n'+names['d']+'\n0\n')==b'','reject wrong network/genesis')
  check(exchange(MAGIC+'\n'+names['d']+'\n9\n')==b'','reject oversized list before allocation')
  check(exchange(MAGIC+'\n'+names['d']+'\n1\n'+names['e']+'\n')==b'','incoming caller cannot inject a list')
  check(state()['known']==2,'malformed messages did not mutate the address book')
  exchange(MAGIC+'\n'+names['d']+'\n0\n');wait(lambda:state()['verified']>=3);check(True,'new self-announcement probed outbound before promotion')
  check((td/'peers.txt').exists(),'address book persisted atomically')
  s=state();check(len(set(s['selected']))==3,'distinct stable synchronization lanes for three reachable services')
  for port in [19024,19026]:
   result=subprocess.run([CORE,'status','127.0.0.1:'+str(port),'--onion-adapter'],text=True,capture_output=True,timeout=20);check(result.returncode==0,'additional independent synchronization lane '+str(port))
  stop_manager();alive.remove(names['b']);proc=start(names['b']);wait(lambda:state()['verified']>=2);check(state()['current'] in (names['c'],names['d']),'restart and reconnect using learned peers while initial contact is offline')
  result=subprocess.run([CORE,'status','127.0.0.1:19022','--onion-adapter'],text=True,capture_output=True,timeout=20);check(result.returncode==0,'native traffic succeeds after bootstrap loss')
  alive.add(names['f']);bad.add(names['f']);exchange(MAGIC+'\n'+names['f']+'\n0\n');time.sleep(1);check(state()['verified']==2,'wrong native network never promoted')
  stop_manager();(td/'peers.txt').write_text('corrupt');proc=start(names['b']);check(state()['known']==1,'corrupt saved address book ignored atomically');stop_manager()
  alive.add(names['b']);proc=start(names['b'],td/'missing-parent/peers.txt');wait(lambda:state()['verified']>=2);wait(lambda:'local address store' in state()['error']);check(state()['verified']>=2,'disk persistence failure does not demote reachable peers');stop_manager()
  for port in [19022,19023,19024,19026,19028]:
   with socket.socket() as c:check(c.connect_ex(('127.0.0.1',port))!=0,'owned listener released '+str(port))
 finally:
  if proc and proc.poll() is None:proc.kill();proc.wait()
  node.terminate();node.wait(timeout=5);stop.set();listener.close()
print('PASS',checks,'onion automatic discovery checks (Tor simulated; native mainnet node real)')
