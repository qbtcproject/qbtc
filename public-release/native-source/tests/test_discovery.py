"""Real local TCP processes: discovery, seed removal, restart, hostile frames.
No router/NAT or external Internet claims. No wallet private keys are required.
"""
import os,pathlib,socket,struct,subprocess,tempfile,time,sys,threading
root=pathlib.Path(__file__).resolve().parents[1]
peer=pathlib.Path(os.environ.get('QBTC_BIN_DIR',root/'build'))/'qbtc-peer'
addr='qbr1pqumxsfhvtyw2n4gxlmkfw3nxtqyvxs0hsaytc9zxkdw0r6gc78sqpl53r3'
def free_port():
 with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]
def exact(s,n):
 b=b''
 while len(b)<n:
  p=s.recv(n-len(b))
  if not p:raise ConnectionError('EOF')
  b+=p
 return b
def frame(p,cmd,payload=b''):
 with socket.create_connection(('127.0.0.1',p),timeout=4) as s:
  s.sendall(struct.pack('<IIBI',0x51425231,1,ord(cmd),len(payload))+payload)
  magic,ver,c,n=struct.unpack('<IIBI',exact(s,13));assert magic==0x51425231 and ver==1 and n<5000
  return chr(c),exact(s,n)
def status(p):
 c,b=frame(p,'S');assert c=='S',b
 return dict(kv.split('=',1) for kv in b.decode().split())
def until(f,seconds=35):
 end=time.monotonic()+seconds
 while time.monotonic()<end:
  try:
   value=f()
   if value:return value
  except (OSError,AssertionError,ConnectionError):pass
  time.sleep(.1)
 raise AssertionError('condition timed out')
with tempfile.TemporaryDirectory(prefix='qbtc-discovery-') as td:
 d=pathlib.Path(td);p=[free_port() for _ in range(3)];c=[free_port() for _ in range(3)];nodes={};logs=[]
 def start(i,seeds):
  log=(d/f'{i}-{len(logs)}.log').open('wb');logs.append(log)
  nodes[i]=subprocess.Popen([peer,'serve',d/f'{i}.db',str(p[i]),'--control-port',str(c[i]),'--discover',*[f'127.0.0.1:{p[j]}' for j in seeds]],stdout=log,stderr=log)
  until(lambda:status(p[i]))
 def stop(i):
  nodes[i].terminate();nodes[i].wait(timeout=10);del nodes[i]
 def mine(i):
  r=subprocess.run([peer,'mine',str(c[i]),addr,'2','--control-file',d/f'{i}.db.control'],capture_output=True,timeout=15)
  assert r.returncode==0,r.stderr
 try:
  start(2,[]);start(1,[2]);start(0,[1])
  until(lambda:all(int(status(x)['peer_verified'])==2 for x in p))
  print('PASS three nodes discover full endpoints from a single initial path',flush=True)
  stop(1);mine(0)
  until(lambda:status(p[2])['height']=='2')
  s0,s2=status(p[0]),status(p[2]);assert s0['tip']==s2['tip'] and s0['utxo_hash']==s2['utxo_hash']
  print('PASS bootstrap node removed; remaining nodes synchronize independently',flush=True)
  stop(0);start(0,[]) # no original seed arguments
  until(lambda:int(status(p[0])['peer_verified'])==2)
  mine(2);until(lambda:status(p[0])['height']=='4')
  assert status(p[0])['tip']==status(p[2])['tip']
  print('PASS durable peer book reconnects after restart with no configured seeds',flush=True)
  before=(d/'0.db.peers-loopback').read_bytes()
  baseline=status(p[0])
  for payload in [struct.pack('<III',17,0,0),struct.pack('<III',1,0x7f000001,0),struct.pack('<III',1,0x7f000001,65536)]:
   command,body=frame(p[0],'D',payload);assert command=='E'
  # Valid framing but a forged self address: must not register arbitrary targets.
  frame(p[0],'D',struct.pack('<III',1,0x7f000002,free_port()))
  # Metadata / private endpoint cannot escape the loopback discovery scope.
  frame(p[0],'D',struct.pack('<III',1,0xa9fea9fe,19001))
  assert status(p[0])['peer_known']==baseline['peer_known']
  assert (d/'0.db.peers-loopback').read_bytes()==before
  print('PASS malformed and spoofed announcements do not poison the persisted book',flush=True)
  # Discovery length is rejected from the header, without waiting for a body.
  with socket.create_connection(('127.0.0.1',p[0]),timeout=3) as s:
   s.sendall(struct.pack('<IIBI',0x51425231,1,ord('D'),4000000))
   started=time.monotonic()
   try:reply=s.recv(300)
   except ConnectionResetError:reply=b''
   assert time.monotonic()-started<2
  # Two partial P2P requests may occupy the per-IP quota. Local control has
  # reserved slots and must still complete a mine request.
  held=[]
  try:
   for _ in range(2):
    s=socket.create_connection(('127.0.0.1',p[0]),timeout=3);s.sendall(b'Q');held.append(s)
   started=time.monotonic();mine(0);assert time.monotonic()-started<4
  finally:
   for s in held:s.close()
  until(lambda:status(p[2])['height']=='6')
  print('PASS oversized discovery header rejected early; P2P stalls cannot occupy control slots',flush=True)
  stop(0)
  # Corrupt local address file must stop startup and remain untouched.
  file=d/'0.db.peers-loopback';bad=bytearray(file.read_bytes());bad[-1]^=1;file.write_bytes(bad)
  r=subprocess.run([peer,'serve',d/'0.db',str(p[0]),'--control-port',str(c[0]),'--discover'],capture_output=True,timeout=10)
  assert r.returncode!=0 and b'peer store checksum' in r.stderr,r.stderr
  assert file.read_bytes()==bad
  print('PASS corrupt peer store prevents startup and preserves original bytes',flush=True)
 finally:
  if sys.exc_info()[0]:
   for i in nodes:
    try:print('NODE STATUS',i,status(p[i]),flush=True)
    except Exception as e:print('NODE ERROR',i,repr(e),flush=True)
   for path in d.glob('*.log'):print(path.name,path.read_text(),flush=True)
  for i in list(nodes):stop(i)
  for f in logs:f.close()
# A fake peer answers the legacy sync commands correctly but announces an
# oversized D response. It must not become a verified, persisted peer.
with tempfile.TemporaryDirectory(prefix='qbtc-hostile-discovery-') as td, socket.socket() as listener:
 listener.bind(('127.0.0.1',0));listener.listen();listener.settimeout(.2)
 fake=listener.getsockname()[1];done=threading.Event();seen=threading.Event();errors=[]
 def hostile():
  while not done.is_set():
   try:connection,_=listener.accept()
   except socket.timeout:continue
   try:
    with connection as s:
     s.settimeout(3)
     magic,version,command,size=struct.unpack('<IIBI',exact(s,13));exact(s,size)
     if command==ord('D'):
      s.sendall(struct.pack('<IIBI',magic,version,command,4000000));seen.set()
     else:s.sendall(struct.pack('<IIBI',magic,version,command,4)+struct.pack('<I',0))
   except Exception as e:errors.append(repr(e))
 thread=threading.Thread(target=hostile);thread.start()
 chain=pathlib.Path(td)/'node.db';p,c=free_port(),free_port()
 process=subprocess.Popen([peer,'serve',chain,str(p),'--control-port',str(c),'--discover',f'127.0.0.1:{fake}'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 try:
  assert seen.wait(10),'D request never reached fake peer'
  time.sleep(.5)
  state=until(lambda:status(p))
  assert state['peer_verified']=='0',state
  assert not pathlib.Path(str(chain)+'.peers-loopback').exists()
  assert not errors,errors
  print('PASS malformed discovery response cannot be silently accepted or persisted',flush=True)
 finally:
  process.terminate();process.wait(timeout=10);done.set();thread.join(timeout=5)
with tempfile.TemporaryDirectory(prefix='qbtc-legacy-discovery-') as td:
 d=pathlib.Path(td);ports=[free_port(),free_port()];controls=[free_port(),free_port()];processes=[]
 try:
  processes.append(subprocess.Popen([peer,'serve',d/'legacy.db',str(ports[0]),'--control-port',str(controls[0])],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL))
  until(lambda:status(ports[0]))
  processes.append(subprocess.Popen([peer,'serve',d/'modern.db',str(ports[1]),'--control-port',str(controls[1]),'--discover',f'127.0.0.1:{ports[0]}'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL))
  until(lambda:status(ports[1])['peer_verified']=='1')
  result=subprocess.run([peer,'mine',str(controls[0]),addr,'2','--control-file',d/'legacy.db.control'],capture_output=True,timeout=10)
  assert result.returncode==0,result.stderr
  until(lambda:status(ports[1])['height']=='2')
  print('PASS explicit discovery-disabled peer reply retains legacy synchronization compatibility',flush=True)
 finally:
  for process in processes:process.terminate()
  for process in processes:process.wait(timeout=10)
print('PASS decentralized discovery integration (local TCP; real WAN remains untested)')
