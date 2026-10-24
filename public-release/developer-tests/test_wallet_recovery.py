"""Actual PowerShell wallet coordinator + native encrypted mainnet wallet on disposable files.
PTY password prompts only; no user wallet, network or rewarded blocks are involved.
"""
import pathlib,os,tempfile,subprocess,pty,select,time,errno,json,shutil,hashlib
ROOT=pathlib.Path(__file__).resolve().parents[1]
BUILD=pathlib.Path(os.environ['QBTC_NATIVE_BUILD']);PWSH=os.environ['QBTC_PWSH'];password=b'Fixture-only-restore-2026!'
def console(args,replies,env):
 master,slave=pty.openpty();proc=subprocess.Popen(list(map(str,args)),stdin=slave,stdout=slave,stderr=slave,env=env);os.close(slave);out=b'';sent=0
 try:
  end=time.monotonic()+20
  while time.monotonic()<end:
   if select.select([master],[],[],.05)[0]:
    try:b=os.read(master,65536)
    except OSError as e:
     if e.errno==errno.EIO:break
     raise
    if not b:break
    out+=b
   if sent<len(replies) and replies[sent][0] in out:os.write(master,replies[sent][1]);sent+=1
   if proc.poll() is not None:break
  proc.wait(timeout=3)
  assert proc.returncode==0,out
  assert password not in out,out
  return out
 finally:
  if proc.poll() is None:proc.kill();proc.wait()
  os.close(master)
with tempfile.TemporaryDirectory(prefix='qbtc-recovery-') as temp:
 t=pathlib.Path(temp);bundle=t/'bundle [restore] 中文';bundle.mkdir();(bundle/'bin').mkdir()
 shutil.copy2(ROOT/'WALLET-ACTION.ps1',bundle/'WALLET-ACTION.ps1');(bundle/'bin/qbtc-wallet.exe').symlink_to(BUILD/'qbtc-wallet')
 env=dict(os.environ,LOCALAPPDATA=str(t/'local é 中文'))
 original=t/'original.qwlt';backup=t/'backup.qwlt'
 out=console([BUILD/'qbtc-wallet','create',original],[(b'New wallet password',password+b'\n'),(b'Repeat password',password+b'\n')],env)
 address=next(line.strip() for line in out.decode().splitlines() if line.startswith('qbc1'))
 console([BUILD/'qbtc-wallet','backup',original,backup],[(b'Wallet password:',password+b'\n')],env)
 def action(name,replies,source=backup):
  request=t/'request.json';request.write_text(json.dumps(dict(action=name,source=str(source))))
  output=console([PWSH,'-NoProfile','-File',bundle/'WALLET-ACTION.ps1','-RequestFile',request],replies,env)
  return json.loads(pathlib.Path(str(request)+'.result').read_text(encoding='utf-8-sig'))
 assert action('verify-backup',[(b'Wallet password:',password+b'\n')])['ok']
 assert action('restore',[(b'Wallet password:',password+b'\n')])['ok']
 wallet=pathlib.Path(env['LOCALAPPDATA'])/'QBTC-test-v2/wallet.qwlt';before=wallet.read_bytes()
 assert action('address',[(b'Wallet password:',password+b'\n')])['ok']
 binding=json.loads((wallet.parent/'wallet-address.json').read_text());assert binding['address']==address and binding['wallet_sha256']==hashlib.sha256(before).hexdigest().upper()
 verified=action('verify-backup',[(b'Wallet password:',password+b'\n')]);assert verified['ok'] and 'matches the active wallet' in verified['text']
 other=t/'other-wallet.qwlt'
 console([BUILD/'qbtc-wallet','create',other],[(b'New wallet password',password+b'\n'),(b'Repeat password',password+b'\n')],env)
 mismatch=action('verify-backup',[(b'Wallet password:',password+b'\n'),(b'Enter to close',b'\n')],other)
 assert not mismatch['ok'] and 'DIFFERENT wallet' in mismatch['text']
 binding_path=wallet.parent/'wallet-address.json';saved=binding_path.read_bytes();binding_path.write_text('{}')
 stale=action('verify-backup',[(b'Wallet password:',password+b'\n'),(b'Enter to close',b'\n')])
 assert not stale['ok'] and 'stale' in stale['text'];binding_path.write_bytes(saved)
 assert not action('restore',[(b'Enter to close',b'\n')])['ok']
 assert wallet.read_bytes()==before and original.exists() and backup.exists()
 print('PASS native encrypted backup verification/restoration through actual PowerShell coordinator, address/hash binding, existing-wallet overwrite refusal, Unicode/bracket paths and no password echo')
