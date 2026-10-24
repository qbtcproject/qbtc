"""Three private TCP endpoints; autonomous local mining survives loss of first node.
Uses distinct loopback IPs on one host, not a claim of physical-machine testing.
"""
import pathlib
import socket
import subprocess
import tempfile
import time

helper = pathlib.Path(__file__).with_name('test_p2p.py')
context = {'__file__': str(helper)}
exec(compile(helper.read_text().split('processes = []')[0], str(helper), 'exec'), context)
peer, wallet = context['peer'], context['wallet']
interactive = context['interactive']
free_ports = context['free_ports']

def status(target):
    text = subprocess.check_output([peer, 'status', target], text=True, timeout=6, stderr=subprocess.DEVNULL)
    return dict(word.split('=', 1) for word in text.split())

def wait_for(predicate, message):
    end = time.monotonic() + 35
    while time.monotonic() < end:
        try:
            if predicate(): return
        except (OSError, subprocess.SubprocessError): pass
        time.sleep(.15)
    raise AssertionError(message)

processes = []
with tempfile.TemporaryDirectory(prefix='qbtc-independent-') as folder:
    directory = pathlib.Path(folder)
    ports = free_ports()
    hosts = ['127.0.0.2', '127.0.0.3', '127.0.0.4']
    targets = [f'{hosts[i]}:{ports[i]}' for i in range(3)]
    paths = [directory / f'node-{i}.db' for i in range(3)]
    wallets, addresses = [], []
    for index in range(3):
        path = directory / f'wallet-{index}'
        result = interactive([wallet, 'create', path], ['New wallet password', 'Repeat password'])
        wallets.append(path)
        addresses.append(next(line.strip() for line in result.splitlines() if line.startswith('qbr1')))
    def launch(index, peers, count=None):
        args = [peer, 'serve', paths[index], str(ports[index]), '--control-port', str(ports[index+3]), '--bind', hosts[index], *peers]
        if count: args += ['--mine', addresses[index], '--mine-count', str(count)]
        process = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        processes.append(process)
        wait_for(lambda: status(targets[index]), 'private endpoint startup')
        return process
    try:
        invalid_path = directory / 'invalid.db'
        for options in [ ['--bind', '0.0.0.0'], ['--bind', '203.0.113.1'], ['8.8.8.8:19001'], ['--mine-count', '1'] ]:
            failed = subprocess.run([peer, 'serve', invalid_path, str(ports[0]), '--control-port', str(ports[3]), *options], capture_output=True, timeout=5)
            assert failed.returncode == 1 and not invalid_path.exists(), failed.stderr
        a = launch(0, [targets[1], targets[2]], 3)
        b = launch(1, [targets[0], targets[2]])
        c = launch(2, [targets[0], targets[1]])
        wait_for(lambda: all(status(target)['height']=='3' for target in targets), 'independent synchronization')
        assert status(targets[0])['auto_mining'] == 'off'
        try:
            with socket.create_connection((hosts[0], ports[3]), timeout=1):
                raise AssertionError('control exposed on peer interface')
        except (ConnectionRefusedError, TimeoutError): pass
        print('PASS independence: own-address automatic mining, numeric peer IPs and local-only control')
        a.terminate(); a.wait(timeout=5)
        b.terminate(); b.wait(timeout=5)
        b = launch(1, [targets[2]], 2)
        wait_for(lambda: status(targets[1])['height']=='5' and status(targets[2])['height']=='5', 'mining continued without first node')
        assert status(targets[1])['utxo_hash'] == status(targets[2])['utxo_hash']
        balance = interactive([wallet, 'balance', wallets[1], paths[1]], ['Wallet password'])
        assert 'total_atoms=10000000000' in balance, balance
        print('PASS independence: first node offline, second mines to its own wallet, third validates identical UTXOs')
        c.terminate(); c.wait(timeout=5)
        paths[2].unlink()
        c = launch(2, [targets[1]])
        wait_for(lambda: status(targets[2])['height']=='5', 'fresh sync without original node')
        assert status(targets[1])['tip'] == status(targets[2])['tip']
        print('PASS independence: empty node joins and synchronizes from another participant with original node offline')
    finally:
        for process in processes:
            if process.poll() is None: process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
            diagnostics = process.stderr.read().decode() if process.stderr else ''
            if diagnostics: print('NODE DIAGNOSTICS:', diagnostics)
print('PASS private endpoint autonomy; no public network launched')
