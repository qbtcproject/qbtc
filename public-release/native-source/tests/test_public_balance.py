"""Public-only CLI checks: closed stdin, no wallet files, read-only state and mature/spent outputs."""
import hashlib
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build-mainnet'
with tempfile.TemporaryDirectory(prefix='qbtc-public-balance-') as temporary:
    folder = pathlib.Path(temporary)
    owner, recipient, main = subprocess.check_output([BUILD / 'balance-fixture', folder], text=True).splitlines()
    def query(binary, address, name, total, spendable):
        path = folder / name
        before = hashlib.sha256(path.read_bytes()).digest()
        result = subprocess.run([BUILD / binary, 'balance-address', address, path], stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=10)
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == f'total_atoms={total} spendable_next_block_atoms={spendable}'
        assert 'password' not in result.stderr.lower()
        assert hashlib.sha256(path.read_bytes()).digest() == before
    query('qbtc-wallet-regtest-balance', owner, 'before.db', 505000000000, 10000000000)
    query('qbtc-wallet-regtest-balance', owner, 'after.db', 507500000000, 12499999000)
    query('qbtc-wallet-regtest-balance', recipient, 'after.db', 2500000000, 2500000000)
    query('qbtc-wallet', main, 'mainnet.db', 0, 0)
    for address, name in [(owner, 'mainnet.db'), (main[:-1] + ('q' if main[-1] != 'q' else 'p'), 'mainnet.db')]:
        result = subprocess.run([BUILD / 'qbtc-wallet', 'balance-address', address, folder / name], stdin=subprocess.DEVNULL, capture_output=True, timeout=5)
        assert result.returncode != 0
    result = subprocess.run([BUILD / 'qbtc-wallet', 'address', folder / 'absent.qwlt'], stdin=subprocess.DEVNULL, capture_output=True, timeout=5)
    assert result.returncode != 0 and b'interactive' in result.stderr
    assert not list(folder.glob('*.qwlt'))
print('PASS public balance without wallet or password; maturity, spent outputs, recipient, checksum/network rejection, read-only storage; private operation still gated')
