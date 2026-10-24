"""Regression: a slow local mining response must not expire like P2P.
Uses a mock listener, no chain or real wallet secrets. Does not test Windows.
"""
import os, pathlib, socket, struct, subprocess, tempfile, threading, time
binary = pathlib.Path(os.environ.get('QBTC_BIN_DIR', 'build')) / 'qbtc-peer'
address = 'qbr1pqumxsfhvtyw2n4gxlmkfw3nxtqyvxs0hsaytc9zxkdw0r6gc78sqpl53r3'

def exact(c,n):
    b=b''
    while len(b)<n:
        p=c.recv(n-len(b))
        if not p: raise RuntimeError('unexpected EOF')
        b+=p
    return b

def exercise(command, delay, reply=True):
    errors=[]; requests=[]
    with socket.socket() as listener, tempfile.TemporaryDirectory() as tmp:
        listener.bind(('127.0.0.1',0));listener.listen();listener.settimeout(8)
        port=listener.getsockname()[1]
        secret=pathlib.Path(tmp)/'test.control';secret.write_bytes(os.urandom(32));secret.chmod(0o600)
        def serve():
            try:
                with listener.accept()[0] as c:
                    c.settimeout(8);h=exact(c,13)
                    net,version,cmd,n=struct.unpack('<IIBI',h)
                    assert net==0x51425231 and version==1 and chr(cmd)==command
                    requests.append(exact(c,n))
                    time.sleep(delay)
                    if reply:
                        payload=b'height=100 mock-response'
                        c.sendall(struct.pack('<IIBI',net,version,cmd,len(payload))+payload)
                # A lost response must never trigger an automatic mining retry.
                listener.settimeout(0.2)
                try:
                    extra=listener.accept()[0];extra.close();raise AssertionError('unexpected retry')
                except socket.timeout: pass
            except (BrokenPipeError,ConnectionResetError):
                if command=='M': errors.append('local mining disconnected early')
            except Exception as e:errors.append(str(e))
        t=threading.Thread(target=serve);t.start()
        args=[str(binary),'mine',str(port),address,'100','--control-file',str(secret)] if command=='M' else [str(binary),'status',str(port)]
        started=time.monotonic();r=subprocess.run(args,capture_output=True,text=True,timeout=10)
        elapsed=time.monotonic()-started;t.join(timeout=10)
        assert not t.is_alive() and not errors,errors
        assert len(requests)==1
        return r,elapsed

r,elapsed=exercise('M',2.2)
assert r.returncode==0 and 'height=100' in r.stdout and elapsed>=2,r
r,elapsed=exercise('S',2.2)
assert r.returncode!=0 and elapsed<2.1,(r,elapsed)
r,elapsed=exercise('M',0,False)
assert r.returncode!=0 and 'blocks may already be committed' in r.stderr and 'does not retry' in r.stderr,r
print('PASS delayed local mining reply; normal P2P timeout retained; lost reply not retried')
