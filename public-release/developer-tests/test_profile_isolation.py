"""Two candidate nodes and one retained test node at height zero; no mining or external network."""
from pathlib import Path
import os,subprocess,tempfile,socket,time,hashlib
root=Path(__file__).resolve().parents[1]/'native-source'
test=root/'build-mainnet';final=root/'build-final'
def port():
 with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]
def status(build,p):return subprocess.run([str(build/'qbtc-peer'),'status',f'127.0.0.1:{p}'],capture_output=True,text=True,timeout=5)
with tempfile.TemporaryDirectory(prefix='qbtc-profile-isolation-') as temp:
 folder=Path(temp)/'chain [isolation] 中文 é';folder.mkdir();nodes=[];ports=[port() for _ in range(3)]
 try:
  for i,build in enumerate([final,final,test]):
   args=[str(build/'qbtc-peer'),'serve',str(folder/f'{i}.db'),str(ports[i]),'--control-port',str(port())]
   if i==1:args+=[f'127.0.0.1:{ports[0]}']
   process=subprocess.Popen(args,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE);nodes.append(process)
   deadline=time.monotonic()+10
   while time.monotonic()<deadline:
    if process.poll() is not None:raise AssertionError(process.stderr.read().decode())
    try:
     result=status(build,ports[i])
     if result.returncode==0 and 'height=0' in result.stdout:break
    except (OSError,subprocess.TimeoutExpired):pass
    time.sleep(.05)
   else:raise AssertionError('Node did not start')
  first=status(final,ports[0]).stdout;second=status(final,ports[1]).stdout
  for key in ['height','tip','utxo_hash']:
   parse=lambda text:dict(x.split('=',1) for x in text.split())[key]
   assert parse(first)==parse(second)
  assert status(test,ports[0]).returncode!=0 and status(final,ports[2]).returncode!=0
 finally:
  for process in nodes:process.terminate()
  for process in nodes:process.wait(timeout=5)
 for own,other,path in [(final,test,folder/'0.db'),(test,final,folder/'2.db')]:
  before=path.read_bytes()
  assert subprocess.run([str(own/'qbtc'),'verify-mainnet',str(path)],capture_output=True).returncode==0
  assert subprocess.run([str(other/'qbtc'),'verify-mainnet',str(path)],capture_output=True).returncode!=0
  assert path.read_bytes()==before
 print('PASS two candidate nodes share height/tip/UTXO hash; retained test and candidate clients/files reject each other; Unicode/bracket chain paths; files unchanged; no rewarded blocks mined')
