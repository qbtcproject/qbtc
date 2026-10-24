"""Linux PTY signing checks on disposable Regtest fixtures; never use a real wallet."""
import pathlib,tempfile,subprocess,os,pty,select,time,hashlib,errno,shutil
build=pathlib.Path(os.environ.get('QBTC_SECURITY_BUILD','/tmp/qbtc-security-asan'))
env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
def run_console(arguments,replies):
 master,slave=pty.openpty();p=subprocess.Popen([str(build/'wallet_cli'),*map(str,arguments)],stdin=slave,stdout=slave,stderr=slave,env=env);os.close(slave)
 output=b'';sent=0;deadline=time.monotonic()+15
 try:
  while time.monotonic()<deadline:
   if select.select([master],[],[],.05)[0]:
    try:data=os.read(master,65536)
    except OSError as e:
     if e.errno==errno.EIO:break
     raise
    if not data:break
    output+=data
   if sent<len(replies) and replies[sent][0] in output:
    os.write(master,replies[sent][1]);sent+=1
   if p.poll() is not None:break
  p.wait(timeout=3)
  while select.select([master],[],[],0)[0]:
   try:output+=os.read(master,65536)
   except OSError:break
  return p.returncode,output
 finally:
  if p.poll() is None:p.kill();p.wait()
  os.close(master)
with tempfile.TemporaryDirectory() as tmp:
 t=pathlib.Path(tmp);address=subprocess.check_output([str(build/'wallet_fixture'),str(t)],env=env,text=True).strip();wallet=t/'fixture.qwlt';chain=t/'chain.db'
 before=(wallet.read_bytes(),chain.read_bytes());password=b'Fixture-only-password-2026!'
 def args(out):return ['send',wallet,chain,address,'100000000','1000',out,'--regtest']
 for response in [b'NO\n',b'SENDextra\n',b'\n']:
  code,output=run_console(args(t/'cancelled.tx'),[(b'Type SEND',response)])
  assert code!=0 and b'nothing signed' in output and b'Wallet password:' not in output and not (t/'cancelled.tx').exists(),output
 code,output=run_console(args(t/'signed.tx'),[(b'Type SEND',b'SEND\n'),(b'Wallet password:',password+b'\n')])
 assert code==0 and (t/'signed.tx').exists() and b'Signed transaction' in output,output
 assert address.encode() in output and b'Amount: 1.00000000 QBTC' in output and b'Fee: 0.00001000 QBTC' in output
 assert password not in output,output
 big=args(t/'oversized.tx');big[4]='65000000000'
 code,output=run_console(big,[(b'Type SEND',b'SEND\n'),(b'Wallet password:',password+b'\n')])
 assert code!=0 and b'relay size policy' in output and not (t/'oversized.tx').exists(),output
 code,output=run_console(['address',wallet],[(b'Wallet password:',b'x'*1025+b'\n')])
 assert code!=0 and b'password length' in output and b'x'*50 not in output,output
 assert (wallet.read_bytes(),chain.read_bytes())==before
 restored=t/'fresh-user-data [restore] 中文 é';restored.mkdir()
 shutil.copy2(t/'backup.qwlt',restored/'restored.qwlt');shutil.copy2(chain,restored/'chain.db')
 assert (t/'backup.qwlt').read_bytes()!=wallet.read_bytes() # independent encryption salt/nonce
 code,output=run_console(['address',restored/'restored.qwlt'],[(b'Wallet password:',password+b'\n')])
 assert code==0 and (t/'owner.txt').read_bytes() in output,output
 fresh=args(restored/'restored-payment.tx');fresh[1]=restored/'restored.qwlt';fresh[2]=restored/'chain.db'
 code,output=run_console(fresh,[(b'Type SEND',b'SEND\n'),(b'Wallet password:',password+b'\n')])
 assert code==0 and password not in output,output
 status=subprocess.check_output([str(build/'wallet_cli'),'payment-status',restored/'chain.db',restored/'restored-payment.tx'],env=env,text=True)
 assert status.startswith('unconfirmed txid=')
 subprocess.run([str(build/'wallet_fixture'),'verify',restored/'chain.db',restored/'restored-payment.tx',restored/'confirmed.db'],env=env,check=True,timeout=10)
 status=subprocess.check_output([str(build/'wallet_cli'),'payment-status',restored/'confirmed.db',restored/'restored-payment.tx'],env=env,text=True)
 assert status.startswith('confirmed height=114 txid=')
 assert (wallet.read_bytes(),chain.read_bytes())==before

 print('PASS signer shows exact recipient/amount/fee; cancellation and overlong confirmation cannot sign; SEND authorizes signing; password hidden; oversized password rejected; wallet/chain unchanged; oversized payment creates no transaction file; encrypted backup restored in fresh Unicode/bracket data folder and spent successfully; native transaction status distinguishes unconfirmed and confirmed')
