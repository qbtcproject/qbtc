"""Three real C++ peer processes, TCP, transactions, partition and signed reorg.
Requires POSIX test_cli.py's PTY helper and the pinned build.
"""
import hashlib
import os
import pathlib
import socket
import struct
import subprocess
import tempfile
import time

# Import only helper definitions; do not execute the CLI test a second time.
helper_source = (pathlib.Path(__file__).with_name("test_cli.py")).read_text()
helper_globals = {"__file__": str(pathlib.Path(__file__).with_name("test_cli.py"))}
exec(compile(helper_source.split('with tempfile.TemporaryDirectory(prefix="qbtc-cli-")')[0], "test_cli.py", "exec"), helper_globals)
interactive = helper_globals["interactive"]
root = pathlib.Path(__file__).resolve().parents[1]
binary_dir = pathlib.Path(os.environ.get("QBTC_BIN_DIR", root / "build"))
peer, wallet = binary_dir / "qbtc-peer", binary_dir / "qbtc-wallet"


def free_ports():
    held = []
    try:
        for _ in range(6):
            sock = socket.socket()
            sock.bind(("127.0.0.1", 0))
            held.append(sock)
        return [s.getsockname()[1] for s in held]
    finally:
        for sock in held:
            sock.close()


def wire_query(port, command, payload=b""):
    def read_exact(sock, n):
        data = b""
        while len(data) < n:
            part = sock.recv(n - len(data))
            assert part, "unexpected EOF"
            data += part
        return data
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        sock.sendall(struct.pack("<IIBI", 0x51425231, 1, ord(command), len(payload)) + payload)
        header = read_exact(sock, 13)
        network, version, response, length = struct.unpack("<IIBI", header)
        assert network == 0x51425231 and version == 1 and response == ord(command)
        return read_exact(sock, length)


def status(port):
    text = subprocess.check_output([peer, "status", str(port)], text=True, stderr=subprocess.DEVNULL, timeout=5)
    return dict(piece.split("=", 1) for piece in text.split())


def wait_for(predicate, label, seconds=35):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            result = predicate()
            if result:
                return result
        except (OSError, subprocess.SubprocessError):
            pass
        time.sleep(0.15)
    diagnostics = []
    for port_value in globals().get("ports", []):
        try:
            diagnostics.append(status(port_value))
        except Exception:
            pass
    raise AssertionError("timeout: " + label + " states=" + str(diagnostics))


def command(*args):
    if len(args) >= 3 and str(args[0]) == str(peer) and args[1] == "mine":
        index = ports.index(int(args[2]))
        args = (args[0], args[1], control_ports[index], *args[3:], "--control-file", str(paths[index]) + ".control")
    return subprocess.check_output([str(x) for x in args], text=True, timeout=30)


processes = []
with tempfile.TemporaryDirectory(prefix="qbtc-p2p-") as name:
    directory = pathlib.Path(name)
    pa, pb, pc, ca, cb, cc = free_ports()
    ports = [pa, pb, pc]
    control_ports = [ca, cb, cc]
    paths = [directory / f"node-{i}.db" for i in range(3)]

    def launch(index, peers):
        process = subprocess.Popen([peer, "serve", paths[index], str(ports[index]), "--control-port", str(control_ports[index]), *map(str, peers)],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        processes.append(process)
        wait_for(lambda: status(ports[index]), "node startup")
        return process

    try:
        wa, wb = directory / "alice.wallet", directory / "bob.wallet"
        a = interactive([wallet, "create", wa], ["New wallet password", "Repeat password"])
        b = interactive([wallet, "create", wb], ["New wallet password", "Repeat password"])
        addr_a = next(line.strip() for line in a.splitlines() if line.startswith("qbr1"))
        addr_b = next(line.strip() for line in b.splitlines() if line.startswith("qbr1"))
        invalid_path = directory / "invalid-ports.db"
        invalid = subprocess.run([peer, "serve", invalid_path, str(pa), "--control-port", str(pa)], capture_output=True, timeout=5)
        assert invalid.returncode == 1 and b"ports must differ" in invalid.stderr
        assert not invalid_path.exists() and not pathlib.Path(str(invalid_path) + ".control").exists()
        na = launch(0, [])
        # Partial headers must occupy only bounded I/O workers, not consensus.
        slow = []
        try:
            for _ in range(8):
                held = socket.create_connection(("127.0.0.1", pa), timeout=3)
                held.sendall(b"Q")
                slow.append(held)
            time.sleep(0.3)
            with socket.create_connection(("127.0.0.1", pa), timeout=2) as extra:
                extra.settimeout(1)
                try:
                    assert extra.recv(1) == b""
                except ConnectionResetError:
                    pass
            started = time.monotonic()
            command(peer, "mine", pa, addr_a, "1")
            assert time.monotonic() - started < 1.5, "partial P2P headers blocked control"
        finally:
            for held in slow: held.close()
        time.sleep(0.3)
        slow_control = []
        try:
            for _ in range(2):
                held = socket.create_connection(("127.0.0.1", ca), timeout=3)
                held.sendall(b"Q")
                slow_control.append(held)
            time.sleep(0.2)
            started = time.monotonic()
            assert status(pa)["height"] == "1", "partial control headers blocked P2P"
            assert time.monotonic() - started < 1.5, "partial control headers delayed P2P"
        finally:
            for held in slow_control: held.close()
        print("PASS P2P: bounded partial-header connections do not block the other endpoint")
        nb, nc = launch(1, [pa, pc]), launch(2, [pb])
        locked = subprocess.run([binary_dir / "qbtc", "mine-to", paths[0], addr_a, "1"], capture_output=True)
        assert locked.returncode == 1 and b"already in use" in locked.stderr
        with socket.create_connection(("127.0.0.1", pa), timeout=3) as sock:
            body = b"\0" * 32 + struct.pack("<I", 1)
            sock.sendall(struct.pack("<IIBI", 0x51425231, 1, ord("M"), len(body)) + body)
            answer = sock.recv(1024)
            assert answer[8:9] == b"E"
        assert status(pa)["height"] == "1"
        # Correctly authenticated mining is rejected on P2P, before command dispatch.
        misplaced = subprocess.run([peer, "mine", str(pa), addr_a, "1", "--control-file", str(paths[0]) + ".control"], capture_output=True, timeout=5)
        assert misplaced.returncode == 1 and b"not permitted on this endpoint" in misplaced.stderr
        # Command policy is enforced from the header, without reading a claimed body.
        for target, forbidden in [(pa, "M"), (ca, "S"), (ca, "B"), (ca, "P"), (ca, "T"), (ca, "A")]:
            with socket.create_connection(("127.0.0.1", target), timeout=2) as sock:
                sock.settimeout(1)
                sock.sendall(struct.pack("<IIBI", 0x51425231, 1, ord(forbidden), 4000000))
                rejection = sock.recv(1024)
                assert rejection[8:9] == b"E"
        print("PASS P2P: control and peer command sets separated before payload reception")
        for target, request, announced in [(pa, "S", 1), (pa, "P", 1), (pa, "A", 3), (pa, "B", 4165), (pa, "T", 4000001), (ca, "M", 109)]:
            with socket.create_connection(("127.0.0.1", target), timeout=3) as sock:
                sock.sendall(struct.pack("<IIBI", 0x51425231, 1, ord(request), announced))
                reply = sock.recv(1024)
                assert reply[8:9] == b"E"
        print("PASS P2P: per-command lengths rejected from header without waiting for payload")
        command(peer, "mine", pa, addr_a, "99")
        wait_for(lambda: all(status(p)["height"] == "100" for p in ports), "initial 100-block synchronization")
        def discovered():
            advertised = wire_query(pa, "A")
            count = struct.unpack_from("<I", advertised)[0]
            values = struct.unpack_from("<" + "I" * count, advertised, 4)
            return pc in values
        wait_for(discovered, "local peer discovery through advertisements")
        tx = directory / "payment.tx"
        interactive([wallet, "send", wa, paths[0], addr_b, "1000000000", "1000", tx, "--regtest"], ["Wallet password"])
        command(peer, "submit", pa, tx)
        wait_for(lambda: all(status(p)["mempool"] == "1" for p in ports), "signed transaction propagation")
        pending_files = [pathlib.Path(str(path) + ".mempool") for path in paths]
        pending_backups = [path.read_bytes() for path in pending_files]
        for process in [na, nb, nc]:
            process.kill(); process.wait(timeout=5)
        # A valid file checksum does not bypass transaction signature validation.
        tampered = bytearray(pending_backups[0])
        tampered[-65] ^= 1
        tampered[-64:] = hashlib.sha3_512(tampered[:-64]).digest()
        pending_files[0].write_bytes(tampered)
        na = launch(0, [])
        assert status(pa)["mempool"] == "0"
        na.terminate(); na.wait(timeout=5)
        pending_files[0].write_bytes(pending_backups[0])
        print("PASS P2P: checksummed pending file cannot bypass ML-DSA signature validation")
        na, nb, nc = launch(0, []), launch(1, []), launch(2, [])
        assert all(status(p)["mempool"] == "1" for p in ports)
        print("PASS P2P: pending signed payment survives abrupt death of all nodes and isolated restarts")
        for process in [na, nb, nc]:
            process.terminate(); process.wait(timeout=5)
        na, nb, nc = launch(0, [pb]), launch(1, [pa, pc]), launch(2, [pb])
        command(peer, "mine", pa, addr_a, "1")
        wait_for(lambda: len({status(p)["tip"] for p in ports}) == 1 and all(status(p)["height"] == "101" for p in ports), "mined signed block propagation")
        states = [status(p) for p in ports]
        assert len({x["utxo_hash"] for x in states}) == 1
        assert all(x["mempool"] == "0" for x in states)
        print("PASS P2P: three processes synchronize blocks, propagate ML-DSA payment and converge UTXOs")
        # Malformed oversized frame is rejected before payload allocation; service survives.
        with socket.create_connection(("127.0.0.1", pc), timeout=3) as sock:
            sock.sendall(struct.pack("<IIBI", 0x51425231, 1, ord("T"), 0xFFFFFFFF))
            response = sock.recv(1024)
            assert response[8:9] == b"E"
        assert status(pc)["height"] == "101"
        print("PASS P2P: malformed frame rejected without corrupting state")
        # Isolate A and C; B stays up on the shared height-101 prefix.
        na.terminate(); na.wait(timeout=5)
        nb.terminate(); nb.wait(timeout=5)
        nc.terminate(); nc.wait(timeout=5)
        # Crash window: chain commit is newer than the last mempool file.
        for pending_path, backup in zip(pending_files, pending_backups):
            pending_path.write_bytes(backup)
        # Restart A without peers, mine its shorter branch in isolation.
        na = launch(0, [])
        nb = launch(1, [])
        assert status(pa)["mempool"] == "0" and status(pb)["mempool"] == "0"
        print("PASS P2P: restart discards stale already-confirmed transactions after chain commit")
        orphan_tx = directory / "orphan-payment.tx"
        interactive([wallet, "send", wb, paths[0], addr_a, "500000000", "2000", orphan_tx, "--regtest"], ["Wallet password"])
        command(peer, "submit", pa, orphan_tx)
        command(peer, "mine", pa, addr_a, "1")
        isolated = status(pa)["tip"]
        command(peer, "mine", pb, addr_b, "2")
        assert status(pb)["height"] == "103"
        na.terminate(); na.wait(timeout=5)
        na = launch(0, [pb])
        nc = launch(2, [pb])
        wait_for(lambda: all(status(p)["height"] == "103" for p in ports) and len({status(p)["tip"] for p in ports}) == 1,
                 "partition healing and cumulative-work reorganization")
        states = [status(p) for p in ports]
        assert states[0]["tip"] != isolated and len({x["utxo_hash"] for x in states}) == 1
        assert all(x["utxos"] == "104" for x in states), states
        bob_balance = interactive([wallet, "balance", wb, paths[0]], ["Wallet password"])
        assert "total_atoms=11000000000" in bob_balance, bob_balance
        print("PASS P2P: partition, divergent signed payment, restart, reorg removes payment and restores funds")
        wait_for(lambda: all(status(p)["mempool"] == "1" for p in ports), "valid detached payment reintroduced and propagated")
        command(peer, "mine", pb, addr_b, "1")
        wait_for(lambda: all(status(p)["height"] == "104" for p in ports) and len({status(p)["utxo_hash"] for p in ports}) == 1,
                 "detached payment can be mined again without double issuance")
        reminted = interactive([wallet, "balance", wb, paths[1]], ["Wallet password"])
        assert "total_atoms=15500000000" in reminted, reminted
        print("PASS P2P: detached payment re-enters mempool and is mined once on the winning chain")
        for process in [na, nb, nc]:
            process.terminate(); process.wait(timeout=5)
        # A failed durable write must stop the node before acknowledging submission.
        failure_tx = directory / "write-failure.tx"
        interactive([wallet, "send", wb, paths[0], addr_a, "100000000", "1000", failure_tx, "--regtest"], ["Wallet password"])
        na = launch(0, [])
        valid_empty = pending_files[0].read_bytes()
        pending_files[0].unlink()
        pending_files[0].mkdir()
        refused = subprocess.run([peer, "submit", str(pa), failure_tx], capture_output=True, timeout=10)
        assert refused.returncode == 1
        assert na.wait(timeout=10) == 1
        pending_files[0].rmdir()
        pending_files[0].write_bytes(valid_empty)
        print("PASS P2P: failed mempool write stops node without acknowledging transaction")
        damaged = bytearray(pending_files[0].read_bytes())
        damaged[-1] ^= 1
        pending_files[0].write_bytes(damaged)
        failed = subprocess.run([peer, "serve", paths[0], str(pa), "--control-port", str(ca)], capture_output=True, timeout=10)
        assert failed.returncode == 1 and b"mempool file checksum" in failed.stderr
        assert pending_files[0].read_bytes() == damaged
        print("PASS P2P: corrupt mempool blocks startup and preserves original file")
    finally:
        for process in processes:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            if process.stderr:
                error_text = process.stderr.read().decode()
                if error_text:
                    print("NODE DIAGNOSTICS:", error_text)
print("PASS real loopback TCP integration; no public testnet launched")
