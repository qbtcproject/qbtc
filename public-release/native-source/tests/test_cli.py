"""POSIX interactive CLI integration. Passwords go only through a pseudo-terminal.
Run after build.sh; no external services or private keys retained.
"""
import os
import pathlib
import pty
import select
import subprocess
import tempfile
import time

root = pathlib.Path(__file__).resolve().parents[1]
binary_dir = pathlib.Path(os.environ.get("QBTC_BIN_DIR", root / "build"))
node = binary_dir / "qbtc"
wallet = binary_dir / "qbtc-wallet"
password = "Ephemeral test-only password 2026!"


def interactive(arguments, prompts):
    master, slave = pty.openpty()
    process = subprocess.Popen([str(x) for x in arguments], stdin=slave, stdout=slave, stderr=slave, close_fds=True)
    os.close(slave)
    output = bytearray()
    sent = 0
    deadline = time.monotonic() + 30
    try:
        while True:
            if time.monotonic() > deadline:
                raise RuntimeError("interactive CLI timeout")
            ready, _, _ = select.select([master], [], [], 0.1)
            if ready:
                try:
                    data = os.read(master, 4096)
                except OSError:
                    data = b""
                if not data:
                    break
                output.extend(data)
                if sent < len(prompts) and prompts[sent].encode() in output:
                    os.write(master, password.encode() + b"\n")
                    sent += 1
            if process.poll() is not None and not ready:
                break
        status = process.wait(timeout=5)
        text = output.decode()
        assert status == 0, text
        assert password not in text, "password echoed by terminal"
        assert sent == len(prompts), text
        return text
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)


def run(*arguments):
    return subprocess.check_output([str(x) for x in arguments], text=True)


def test_password_prompt_eof(path):
    import termios
    master, slave = pty.openpty()
    original = termios.tcgetattr(slave)
    process = subprocess.Popen([str(wallet), "create", str(path)], stdin=slave, stdout=slave, stderr=slave, close_fds=True)
    output = b""
    try:
        deadline = time.monotonic() + 10
        while b"New wallet password" not in output:
            assert time.monotonic() < deadline, "password prompt timeout"
            ready, _, _ = select.select([master], [], [], 0.1)
            if ready:
                output += os.read(master, 4096)
        assert not termios.tcgetattr(slave)[3] & termios.ECHO, "prompt printed before echo disabled"
        os.write(master, b"\x04")
        assert process.wait(timeout=5) == 1
        assert bool(termios.tcgetattr(slave)[3] & termios.ECHO) == bool(original[3] & termios.ECHO)
        assert not path.exists()
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)
        os.close(slave)


with tempfile.TemporaryDirectory(prefix="qbtc-cli-") as name:
    directory = pathlib.Path(name)
    test_password_prompt_eof(directory / "cancelled.wallet")
    alice, bob = directory / "alice.wallet", directory / "bob.wallet"
    chain, tx, backup = directory / "chain.db", directory / "tx.bin", directory / "backup.wallet"
    a = interactive([wallet, "create", alice], ["New wallet password", "Repeat password"])
    b = interactive([wallet, "create", bob], ["New wallet password", "Repeat password"])
    addr_a = next(line.strip() for line in a.splitlines() if line.startswith("qbr1"))
    addr_b = next(line.strip() for line in b.splitlines() if line.startswith("qbr1"))
    assert addr_a != addr_b
    assert "height=101" in run(node, "mine-to", chain, addr_a, "101")
    print("PASS CLI: two encrypted wallets and 101 owned coinbase blocks")
    interactive([wallet, "send", alice, chain, addr_b, "1000000000", "1000", tx, "--regtest"], ["Wallet password"])
    assert "height=102" in run(node, "mine-tx", chain, addr_a, tx)
    balance = interactive([wallet, "balance", bob, chain], ["Wallet password"])
    assert "total_atoms=1000000000" in balance, balance
    history = interactive([wallet, "history", bob, chain], ["Wallet password"])
    assert "height=102" in history and "received_atoms=1000000000" in history
    print("PASS CLI: signed payment, mining, recipient balance and history")
    interactive([wallet, "backup", alice, backup], ["Wallet password"])
    recovered = interactive([wallet, "address", backup], ["Wallet password"])
    assert addr_a in recovered
    original_balance = interactive([wallet, "balance", alice, chain], ["Wallet password"])
    restored_balance = interactive([wallet, "balance", backup, chain], ["Wallet password"])
    assert original_balance == restored_balance
    assert "Verified height=102" in run(node, "verify-regtest", chain)
    explored = run(node, "block", chain, "102")
    assert "height=102" in explored and "transactions=2" in explored and "value_atoms=1000000000" in explored
    before = chain.read_bytes()
    replay = subprocess.run([node, "mine-tx", chain, addr_a, tx], capture_output=True)
    assert replay.returncode != 0 and chain.read_bytes() == before
    print("PASS CLI: encrypted recovery, full chain restart and replay rejection")
print("PASS end-to-end CLI (passwords not echoed, no private keys retained)")
