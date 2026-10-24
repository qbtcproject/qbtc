"""Local socket tests of Internet opt-in; no real WAN/NAT coverage."""
import pathlib, tempfile, subprocess, socket, time, struct, os
root=pathlib.Path(__file__).resolve().parents[1]
peer=pathlib.Path(os.environ.get('QBTC_BIN_DIR',root/'build'))/'qbtc-peer'
def port():
 with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]
def request(host,p,cmd):
 with socket.create_connection((host,p),timeout=2) as s:
  # Same regtest header as the existing integration suite.
  s.sendall(struct.pack('<IIBI',0x51425231,1,ord(cmd),0))
  def exact(n):
   data=b''
   while len(data)<n:
    part=s.recv(n-len(data))
    if not part:raise ConnectionError('closed')
    data+=part
   return data
  header=exact(13)
  magic,version,command,size=struct.unpack('<IIBI',header)
  assert size<=4096
  return exact(size)
with tempfile.TemporaryDirectory() as td:
 d=pathlib.Path(td)
 for args in [['--internet-test'],['8.8.8.8:19001'],['--internet-test','--bind','0.0.0.0','8.8.8.8:19001'],['--internet-test','100.64.0.1:19001']]:
  c=d/'rejected.db'
  r=subprocess.run([peer,'serve',c,str(port()),'--control-port',str(port()),*args],capture_output=True,timeout=5)
  assert r.returncode!=0,(args,r.stdout,r.stderr)
  assert not c.exists(),args
 print('PASS Internet preflight rejects missing peers, implicit public peers, wildcard bind and CGNAT before chain creation')
with tempfile.TemporaryDirectory() as td:
 d=pathlib.Path(td); procs=[]
 try:
  ports=[port(),port()]; controls=[port(),port()]
  for i in range(2):
   procs.append(subprocess.Popen([peer,'serve',d/f'{i}.db',str(ports[i]),'--control-port',str(controls[i]),'--internet-test',f'127.0.0.1:{ports[1-i]}'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL))
  deadline=time.monotonic()+10
  while True:
   try:
    for p in ports: assert b'height=0' in request('127.0.0.1',p,'S')
    break
   except (OSError,AssertionError):
    assert time.monotonic()<deadline
    time.sleep(.05)
  a='qbr1pqumxsfhvtyw2n4gxlmkfw3nxtqyvxs0hsaytc9zxkdw0r6gc78sqpl53r3'
  r=subprocess.run([peer,'mine',str(controls[0]),a,'2','--control-file',d/'0.db.control'],capture_output=True,timeout=10)
  assert r.returncode==0,r.stderr
  deadline=time.monotonic()+10
  while b'height=2' not in request('127.0.0.1',ports[1],'S'):
   assert time.monotonic()<deadline
   time.sleep(.1)
  for p in ports:
   try: answer=request('127.0.0.1',p,'M')
   except OSError: answer=b''
   assert b'height=' not in answer
  print('PASS opt-in peers synchronize mined blocks; P2P rejects mining control')
 finally:
  for p in procs:p.terminate()
  for p in procs:p.wait(timeout=10)
