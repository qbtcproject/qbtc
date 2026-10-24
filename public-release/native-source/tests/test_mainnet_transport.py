"""Mainnet profile at height zero. No rewarded mainnet blocks are mined."""
import pathlib,subprocess,tempfile,socket,struct,time,os
root=pathlib.Path(__file__).resolve().parents[1]
peer=root/'build-mainnet/qbtc-peer'
regpeer=root/'build/qbtc-peer'
def port():
 with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]
def exact(s,n):
 b=b''
 while len(b)<n:
  part=s.recv(n-len(b))
  if not part:raise ConnectionError('closed')
  b+=part
 return b
def request(p,network,cmd='S',payload=b''):
 with socket.create_connection(('127.0.0.1',p),timeout=4) as s:
  s.sendall(struct.pack('<IIBI',network,1,ord(cmd),len(payload))+payload)
  magic,version,c,n=struct.unpack('<IIBI',exact(s,13));assert n<4096
  return magic,chr(c),exact(s,n)
def wait(f):
 deadline=time.monotonic()+20
 while time.monotonic()<deadline:
  try:
   if f():return
  except (OSError,AssertionError):pass
  time.sleep(.1)
 raise AssertionError('mainnet condition timed out')
with tempfile.TemporaryDirectory(prefix='qbtc-mainnet-zero-') as td:
 d=pathlib.Path(td);ports=[port(),port()];control=[port(),port()];nodes=[]
 try:
  for i in range(2):
   args=[peer,'serve',d/f'{i}.db',str(ports[i]),'--control-port',str(control[i]),'--discover']
   if i:args+=[f'127.0.0.1:{ports[0]}']
   nodes.append(subprocess.Popen(args,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL))
   wait(lambda:b'height=0' in request(ports[i],0x51424d32)[2])
  wait(lambda:all(b'peer_verified=1' in request(p,0x51424d32)[2] for p in ports))
  for p in ports:
   magic,cmd,state=request(p,0x51424d32)
   assert magic==0x51424d32 and cmd=='S' and b'network=mainnet' in state and b'utxos=0' in state
   _,cmd,error=request(p,0x51425231)
   assert cmd=='E' and b'wrong network' in error
   r=subprocess.run([regpeer,'status',str(p)],capture_output=True,timeout=5)
   assert r.returncode!=0 and b'wrong network' in r.stderr
  print('PASS mainnet P2P identity and two-node discovery at height zero')
  print('PASS regtest frames and regtest client rejected by mainnet')
 finally:
  for n in nodes:n.terminate()
  for n in nodes:n.wait(timeout=10)
print('PASS no rewarded mainnet blocks mined or distributed')
