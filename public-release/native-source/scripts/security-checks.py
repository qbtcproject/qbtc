"""Linux ASan/UBSan regression runner. Uses a separately built, verified OpenSSL 3.5.9.
Example: python scripts/security-checks.py --openssl /absolute/openssl-3.5.9 --output /tmp/qbtc-security
LeakSanitizer is opt-in because some executors cannot inspect /proc/<pid>/task.
"""
import argparse,json,os,pathlib,subprocess,sys
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--openssl',type=pathlib.Path,required=True)
p.add_argument('--output',type=pathlib.Path,required=True)
p.add_argument('--compiler',default='g++')
p.add_argument('--leak-check',action='store_true')
a=p.parse_args();root=pathlib.Path(__file__).resolve().parents[1];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
ssl=a.openssl.resolve()
flags=['-std=c++20','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(root/'include'),'-I'+str(ssl/'include'),'-pthread','-Wall','-Wextra','-Wconversion','-Wsign-conversion','-Wpedantic','-Werror']
common=['consensus','utxo','journal','storage','lock','address','wallet','control','net','peers','mempool']
for n in common:subprocess.run([a.compiler,*flags,'-c',str(root/'src'/(n+'.cpp')),'-o',str(out/(n+'.o'))],check=True)
suites=['consensus','wallet','journal','control','fuzz','mempool','internet_policy','peers','parallel_mining','difficulty_launch','security_admission','request_budget','node_policy'];result=[]
for n in suites:
 subprocess.run([a.compiler,*flags,str(root/'tests'/('test_'+n+'.cpp')),*[str(out/(x+'.o')) for x in common],str(ssl/'libcrypto.a'),'-o',str(out/n)],check=True)
 env=dict(os.environ,ASAN_OPTIONS='detect_leaks='+str(int(a.leak_check))+':halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
 r=subprocess.run([str(out/n)],capture_output=True,text=True,env=env,timeout=180)
 result.append(dict(suite=n,exit_code=r.returncode,stdout=r.stdout,stderr=r.stderr));print(n,r.returncode,r.stdout,r.stderr,flush=True)
for name, entry in [('wallet_cli','src/wallet_main.cpp'),('wallet_fixture','tests/security_wallet_fixture.cpp')]:
 subprocess.run([a.compiler,*flags,str(root/entry),*[str(out/(x+'.o')) for x in common],str(ssl/'libcrypto.a'),'-o',str(out/name)],check=True)
console=subprocess.run([sys.executable,str(root.parent/'developer-tests/test_signer_confirmation.py')],env=dict(env,QBTC_SECURITY_BUILD=str(out)),capture_output=True,text=True,timeout=90)
result.append(dict(suite='signer_console',exit_code=console.returncode,stdout=console.stdout,stderr=console.stderr))
print(console.stdout,console.stderr,flush=True)
(out/'results.json').write_text(json.dumps({'leak_sanitizer_enabled':a.leak_check,'suites':result},indent=2))
raise SystemExit(int(any(r['exit_code'] for r in result)))
