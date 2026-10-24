"""Disposable local nodes only. No live files, Tor services or blocks are used."""
import os,pathlib,socket,struct,subprocess,tempfile,time
peer=os.environ['QBTC_NATIVE_PEER'];magic=0x51424d32

def port():
 with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]
def exact(s,n):
 out=b''
 while len(out)<n:
  part=s.recv(n-len(out))
  if not part:raise EOFError()
  out+=part
 return out
def request(p,command,size=0,body=b''):
 with socket.create_connection(('127.0.0.1',p),timeout=5) as s:
  s.sendall(struct.pack('<IIBI',magic,1,ord(command),size)+body)
  network,version,c,n=struct.unpack('<IIBI',exact(s,13))
  assert network==magic and version==1 and n<=2048
  return chr(c),exact(s,n)
with tempfile.TemporaryDirectory() as tmp:
 p,c=port(),port();chain=str(pathlib.Path(tmp)/'chain.db')
 log=open(pathlib.Path(tmp)/'peer.log','w')
 node=subprocess.Popen([peer,'serve',chain,str(p),'--control-port',str(c),'--onion-adapter','127.0.0.1:19022'],stdout=log,stderr=log)
 try:
  end=time.monotonic()+10
  while True:
   try:before=request(p,'S');break
   except (OSError,EOFError):
    if time.monotonic()>end:raise
    time.sleep(.05)
  start=time.monotonic();command,reason=request(p,'T',65537)
  assert command=='E' and b'request length invalid' in reason and time.monotonic()-start<2
  assert request(p,'A',4,struct.pack('<I',19050))==('A',struct.pack('<I',0))
  assert request(p,'M',108)[0]=='E' # never allow mining control on the public endpoint
  assert request(p,'S')==before and node.poll() is None
  # Exhaust public read quota directly, then prove authenticated local health queries still work.
  from concurrent.futures import ThreadPoolExecutor
  with ThreadPoolExecutor(max_workers=8) as workers:
   responses=list(workers.map(lambda _:request(p,'S'),range(96)))
  assert any(command=='E' and b'rate limit' in body for command,body in responses)
  result=subprocess.run([peer,'local-status',str(c),'--control-file',chain+'.control'],capture_output=True,text=True,timeout=10)
  assert result.returncode==0 and 'height=0' in result.stdout,result
  # A forged authenticated request must fail on the control endpoint.
  assert request(c,'S',72,bytes(72))[0]=='E'

 finally:node.terminate();node.wait(timeout=5);log.close()
 if os.name=='posix':
  fds=[]
  try:
   while not fds or fds[-1]<1030:fds.append(os.open('/dev/null',os.O_RDONLY))
   result=subprocess.run([peer,'serve',str(pathlib.Path(tmp)/'high-fd.db'),str(port()),'--control-port',str(port())],pass_fds=fds,capture_output=True,text=True,timeout=10)
   assert result.returncode==1 and 'listen descriptor limit' in result.stderr,result
  finally:
   for fd in fds:os.close(fd)
 print('PASS header-only oversized submit rejection; public mining and onion localhost injection blocked; public quota enforced; authenticated local status survives; forged control rejected; state unchanged; high-fd listeners fail safely')
