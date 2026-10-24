"""Cross-build Windows x64 using a verified LLVM-MinGW and OpenSSL source build.
Does not run the generated executables. No network access in this script.
"""
import os
import pathlib
import subprocess

root = pathlib.Path(__file__).resolve().parents[1]
os.chdir(root)
compiler = pathlib.Path(os.environ["LLVM_MINGW_ROOT"]) / "bin/x86_64-w64-mingw32-g++"
crypto_build = pathlib.Path(os.environ["OPENSSL_WIN_BUILD"])
crypto_source = pathlib.Path(os.environ["OPENSSL_WIN_SOURCE"])
build = root / "build-windows"
build.mkdir(exist_ok=True)
flags = ["-std=c++20", "-O2", "-Wall", "-Wextra", "-Wconversion", "-Wsign-conversion", "-Wpedantic", "-Werror",
         "-D_WIN32_WINNT=0x0A00", "-Iinclude", f"-I{crypto_build}/include", f"-I{crypto_source}/include"]
for source in ["consensus", "utxo", "journal", "storage", "lock", "address", "control", "net", "peers", "wallet", "mempool"]:
    print("Windows compile", source, flush=True)
    subprocess.run([compiler, *flags, "-c", f"src/{source}.cpp", "-o", build / f"{source}.o"], check=True)
core = [build / f"{source}.o" for source in ["consensus", "utxo", "journal", "storage", "lock", "address"]]
link = ["-municode", crypto_build / "libcrypto.a", "-static", "-lws2_32", "-ladvapi32", "-lcrypt32", "-lbcrypt", "-luser32"]
targets = [
    ("qbtc", ["src/main.cpp"]),
    ("qbtc-wallet", [build / "wallet.o", "src/wallet_main.cpp"]),
    ("qbtc-peer", [build / "net.o", build / "peers.o", build / "control.o", build / "mempool.o", "src/peer_main.cpp"]),
    ("qbtc-peers-tests", [build / "net.o", build / "peers.o", "tests/test_peers.cpp"]),
    ("qbtc-network-tests", [build / "net.o", build / "peers.o", "tests/test_internet_policy.cpp"]),
    ("qbtc-tests", ["tests/test_consensus.cpp"]),
    ("qbtc-wallet-tests", [build / "wallet.o", "tests/test_wallet.cpp"]),
    ("qbtc-journal-tests", ["tests/test_journal.cpp"]),
    ("qbtc-control-tests", [build / "control.o", "tests/test_control.cpp"]),
    ("qbtc-mempool-tests", [build / "mempool.o", "tests/test_mempool.cpp"]),
    ("qbtc-fuzz-tests", ["tests/test_fuzz.cpp"]),
]
for name, extra in targets:
    print("Windows link", name, flush=True)
    target_link=[x for x in link if x!="-municode" or name in ("qbtc","qbtc-wallet","qbtc-peer")]
    subprocess.run([compiler, *flags, *core, *extra, *target_link, "-o", build / f"{name}.exe"], check=True)
for name, _ in targets:
    subprocess.run([compiler.parent / "llvm-strip", "--strip-all", build / f"{name}.exe"], check=True)
print("Windows x64 cross-build completed; release symbols stripped; executables NOT run", flush=True)
