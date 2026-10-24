#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
cxx="${CXX:-g++}"
crypto_root="${OPENSSL_ROOT:-$PWD/.deps/openssl-3.5.9}"
if [[ ! -f "$crypto_root/libcrypto.a" ]]; then
 echo 'Pinned OpenSSL not built. Run scripts/bootstrap-openssl.sh or set OPENSSL_ROOT to its source/build directory.' >&2
 exit 1
fi
flags=(-std=c++20 -Wall -Wextra -Wconversion -Wsign-conversion -Wpedantic -Werror -Iinclude -I"$crypto_root/include")
link=("$crypto_root/libcrypto.a" -ldl -pthread)
if [[ "${SANITIZE:-0}" == 1 ]]; then
 flags+=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
else
 flags+=(-O2)
fi
"$cxx" "${flags[@]}" -c src/mempool.cpp -o build/mempool.o
"$cxx" "${flags[@]}" -c src/peers.cpp -o build/peers.o
"$cxx" "${flags[@]}" -c src/net.cpp -o build/net.o
"$cxx" "${flags[@]}" -c src/control.cpp -o build/control.o
"$cxx" "${flags[@]}" -c src/lock.cpp -o build/lock.o
"$cxx" "${flags[@]}" -c src/storage.cpp -o build/storage.o
"$cxx" "${flags[@]}" -c src/journal.cpp -o build/journal.o
"$cxx" "${flags[@]}" -c src/utxo.cpp -o build/utxo.o
"$cxx" "${flags[@]}" -c src/consensus.cpp -o build/consensus.o
"$cxx" "${flags[@]}" -c src/address.cpp -o build/address.o
"$cxx" "${flags[@]}" -c src/wallet.cpp -o build/wallet.o
"$cxx" "${flags[@]}" build/consensus.o build/utxo.o build/journal.o build/storage.o build/address.o build/lock.o src/main.cpp "${link[@]}" -o build/qbtc
"$cxx" "${flags[@]}" build/consensus.o build/utxo.o build/journal.o build/storage.o build/address.o build/lock.o build/control.o build/net.o build/peers.o build/mempool.o src/peer_main.cpp "${link[@]}" -o build/qbtc-peer
"$cxx" "${flags[@]}" build/consensus.o build/utxo.o build/journal.o build/storage.o build/address.o build/wallet.o src/wallet_main.cpp "${link[@]}" -o build/qbtc-wallet
"$cxx" "${flags[@]}" build/consensus.o build/utxo.o build/journal.o build/storage.o tests/test_consensus.cpp "${link[@]}" -o build/qbtc-tests
"$cxx" "${flags[@]}" build/consensus.o build/utxo.o build/journal.o build/storage.o build/address.o build/wallet.o tests/test_wallet.cpp "${link[@]}" -o build/qbtc-wallet-tests
"$cxx" "${flags[@]}" build/consensus.o build/utxo.o build/journal.o build/storage.o build/address.o build/wallet.o tests/crypto_interop.cpp "${link[@]}" -o build/qbtc-crypto-interop
"$cxx" "${flags[@]}" build/consensus.o build/utxo.o build/journal.o build/storage.o tests/test_journal.cpp "${link[@]}" -o build/qbtc-journal-tests
"$cxx" "${flags[@]}" -DQBTC_DELTAS build/consensus.o build/utxo.o build/journal.o build/storage.o tests/benchmark_chain.cpp "${link[@]}" -o build/qbtc-benchmark
"$cxx" "${flags[@]}" build/consensus.o build/utxo.o build/journal.o build/storage.o build/control.o tests/test_control.cpp "${link[@]}" -o build/qbtc-control-tests
"$cxx" "${flags[@]}" build/consensus.o build/utxo.o build/journal.o build/storage.o tests/test_fuzz.cpp "${link[@]}" -o build/qbtc-fuzz-tests
chmod +x build/qbtc build/qbtc-peer build/qbtc-wallet build/qbtc-tests build/qbtc-wallet-tests build/qbtc-crypto-interop build/qbtc-journal-tests build/qbtc-control-tests build/qbtc-fuzz-tests build/qbtc-benchmark
./build/qbtc-tests
./build/qbtc-wallet-tests
./build/qbtc-journal-tests
./build/qbtc-control-tests
./build/qbtc-fuzz-tests

"$cxx" "${flags[@]}" build/consensus.o build/utxo.o build/journal.o build/storage.o build/mempool.o tests/test_mempool.cpp "${link[@]}" -o build/qbtc-mempool-tests
chmod +x build/qbtc-mempool-tests
./build/qbtc-mempool-tests

"$cxx" "${flags[@]}" build/consensus.o build/utxo.o build/journal.o build/storage.o build/address.o build/lock.o build/net.o build/peers.o tests/test_internet_policy.cpp "${link[@]}" -o build/qbtc-network-tests
./build/qbtc-network-tests

"$cxx" "${flags[@]}" build/consensus.o build/utxo.o build/journal.o build/storage.o build/address.o build/lock.o build/net.o build/peers.o tests/test_peers.cpp "${link[@]}" -o build/qbtc-peers-tests
./build/qbtc-peers-tests
