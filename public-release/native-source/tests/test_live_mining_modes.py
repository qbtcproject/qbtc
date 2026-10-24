"""Private disposable chains only; never connects to live peers or uses a real wallet."""
import pathlib, socket, struct, subprocess, tempfile, time

ROOT = pathlib.Path(__file__).resolve().parents[1]
PEER = ROOT / 'build-mainnet/qbtc-peer'
fixture = subprocess.check_output([ROOT / 'build-mainnet/qbtc-parallel-mining-tests'], text=True)
ADDRESS = next(x.split('=', 1)[1] for x in fixture.splitlines() if x.startswith('fixture_address='))

def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]

def exact(s, n):
    result = b''
    while len(result) < n:
        part = s.recv(n - len(result))
        if not part:
            raise EOFError()
        result += part
    return result

def status(p):
    with socket.create_connection(('127.0.0.1', p), timeout=5) as s:
        s.sendall(struct.pack('<IIBI', 0x51424d32, 1, ord('S'), 0))
        network, version, command, n = struct.unpack('<IIBI', exact(s, 13))
        assert (network, version, command) == (0x51424d32, 1, ord('S')) and n < 4096
        return dict(x.split('=', 1) for x in exact(s, n).decode().split())

def wait(check, seconds=30):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            result = check()
            if result:
                return result
        except (OSError, EOFError):
            pass
        time.sleep(.1)
    raise AssertionError('mining condition timed out')

with tempfile.TemporaryDirectory(prefix='qbtc-discarded-mining-') as tmp:
    d = pathlib.Path(tmp)
    ports = [port(), port()]
    controls = [port(), port()]
    mode = d / 'mode.txt'
    mode.write_text('low')
    nodes = []
    try:
        for i in range(2):
            args = [str(PEER), 'serve', str(d / f'{i}.db'), str(ports[i]), '--control-port', str(controls[i])]
            if not i:
                args += ['--mine', ADDRESS, '--mining-mode', 'low', '--mining-mode-file', str(mode)]
            else:
                args += [f'127.0.0.1:{ports[0]}']
            nodes.append(subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
            wait(lambda: status(ports[i]))
        wait(lambda: int(status(ports[0])['last_hash_attempts']) > 10000)
        assert int(status(ports[0])['last_pipeline_hps']) > 2560
        for selected in ['medium', 'high', 'low']:
            mode.write_text(selected)
            wait(lambda: status(ports[0])['mining_mode'] == selected)
            assert nodes[0].poll() is None
        mode.write_text('invalid or partial setting')
        time.sleep(1.2)
        assert status(ports[0])['mining_mode'] == 'low'
        wait(lambda: int(status(ports[0])['height']) >= 1, 60)
        # Disable only this disposable miner by terminating it, then restart as a node.
        nodes[0].terminate(); nodes[0].wait(timeout=10)
        nodes[0] = subprocess.Popen([str(PEER), 'serve', str(d / '0.db'), str(ports[0]), '--control-port', str(controls[0])], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wait(lambda: status(ports[0]))
        wait(lambda: status(ports[0])['tip'] == status(ports[1])['tip'])
        a, b = status(ports[0]), status(ports[1])
        assert a['utxo_hash'] == b['utxo_hash'] and a['height'] == b['height']
        assert a['auto_mining'] == 'off' and a['mining_threads'] == '0'
        print('PASS live mode changes without restart, invalid preference ignored, continuous hashrate, responsive P2P, mined block sync, restart recovery')
    finally:
        for node in nodes:
            if node.poll() is None:
                node.terminate()
            node.wait(timeout=10)
