#include "qbtc/platform.hpp"
#include "qbtc/address.hpp"
#include "qbtc/consensus.hpp"
#include "qbtc/lock.hpp"
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <openssl/crypto.h>
using namespace qbtc;
namespace {
std::uint64_t count(std::string_view s) {
    std::uint64_t n = 0;
    auto [p, e] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (e != std::errc{} || p != s.data() + s.size() || n > 10000)
        throw Invalid("count must be 0..10000");
    return n;
}
Bytes txfile(const std::string &path) {
    std::ifstream f(file_path(path), std::ios::binary | std::ios::ate);
    if (!f)
        throw Invalid("transaction file open");
    auto n = f.tellg();
    if (n < 0 || n > MAX_BLOCK_BYTES)
        throw Invalid("transaction file size");
    Bytes b(static_cast<std::size_t>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char *>(b.data()), static_cast<std::streamsize>(b.size()));
    if (!f)
        throw Invalid("transaction file read");
    return b;
}
} // namespace
int qbtc_entry(int argc, char **argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "info") {
            auto g = genesis(ACTIVE_NETWORK);
            std::cout << "QBTC " << VERSION << " network=" << NETWORK_NAME << "\nOpenSSL "
                      << OpenSSL_version(OPENSSL_VERSION) << "\nGenesis " << hex(blockid(g.header))
                      << "\nMaximum issuance in atomic units " << total_emission()
                      << "\nML-DSA-65 backend "
                      << (crypto_available() ? "available (not certified)"
                                             : "unavailable; node operations disabled")
                      << "\n";
            return 0;
        }
        if (!crypto_available())
            throw Invalid("node requires ML-DSA-65; rebuild with pinned OpenSSL");
        if (ACTIVE_NETWORK == Network::Regtest && argc == 4 && std::string(argv[1]) == "mine-regtest") {
            auto blocks = count(argv[3]);
            storage::RunLock lock(argv[2]);
            Chain c(ACTIVE_NETWORK);
            Address recipient{};
            c.append(argv[2]);
            std::uint64_t attempts = 0;
            for (std::uint64_t i = 0; i < blocks; i++) {
                auto b = candidate(c, recipient, c.tip_header().timestamp + SPACING);
                attempts += mine(b, 1000000);
                c.add(b);
                c.append(argv[2]);
            }
            c.append(argv[2]);
            std::cout << "height=" << c.tip_header().height << " hash_attempts=" << attempts
                      << " tip=" << hex(blockid(c.tip_header()))
                      << "\nRewards use an unowned regtest address.\n";
            return 0;
        }
        if (ACTIVE_NETWORK == Network::Regtest && argc == 5 && (std::string(argv[1]) == "mine-to" || std::string(argv[1]) == "mine-tx")) {
            auto recipient = wallet::parse_address(argv[3]);
            storage::RunLock lock(argv[2]);
            Chain c = std::filesystem::exists(file_path(argv[2])) ? Chain::load(ACTIVE_NETWORK, argv[2])
                                                       : Chain(ACTIVE_NETWORK);
            std::vector<Transaction> txs;
            std::uint64_t blocks = 1;
            if (std::string(argv[1]) == "mine-tx")
                txs.push_back(decode_transaction(txfile(argv[4])));
            else
                blocks = count(argv[4]);
            c.append(argv[2]);
            std::uint64_t attempts = 0;
            for (std::uint64_t i = 0; i < blocks; i++) {
                auto b = candidate(c, recipient, c.tip_header().timestamp + SPACING, txs);
                attempts += mine(b, 1000000);
                c.add(b);
                c.append(argv[2]);
            }
            c.append(argv[2]);
            std::cout << "height=" << c.tip_header().height << " hash_attempts=" << attempts
                      << " tip=" << hex(blockid(c.tip_header())) << "\n";
            return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "block") {
            auto chain = Chain::load(ACTIVE_NETWORK, argv[2]);
            std::uint64_t height = 0;
            std::string_view text = argv[3];
            auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), height);
            if (error != std::errc{} || end != text.data() + text.size() ||
                height > chain.tip_header().height)
                throw Invalid("invalid explorer height");
            auto cursor = blockid(chain.tip_header());
            while (chain.header(cursor).height > height)
                cursor = chain.header(cursor).previous;
            const auto &b = chain.block(cursor);
            std::cout << "height=" << height << " hash=" << hex(cursor)
                      << " timestamp=" << b.header.timestamp << " nonce=" << b.header.nonce
                      << " target=" << hex(b.header.target)
                      << " transactions=" << b.transactions.size() << "\n";
            for (const auto &tx : b.transactions) {
                std::cout << "tx=" << hex(txid(tx)) << " inputs=" << tx.inputs.size()
                          << " coinbase_height=" << tx.coinbase_height << "\n";
                for (const auto &output : tx.outputs)
                    std::cout << "  value_atoms=" << output.value
                              << " address=" << wallet::address(output.owner) << "\n";
            }
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) ==
            (ACTIVE_NETWORK == Network::Mainnet ? "verify-mainnet" : "verify-regtest")) {
            auto c = Chain::load(ACTIVE_NETWORK, argv[2]);
            std::cout << "Verified height=" << c.tip_header().height
                      << " UTXOs=" << c.utxos().size() << "\n";
            return 0;
        }
        if (ACTIVE_NETWORK == Network::Mainnet) {
            std::cerr << "Usage: qbtc info | verify-mainnet CHAIN | block CHAIN HEIGHT\n";
            return 2;
        }
        std::cerr << "Usage: qbtc info | mine-regtest FILE COUNT | verify-regtest FILE | block "
                     "CHAIN HEIGHT\n"
                  << "       qbtc mine-to CHAIN ADDRESS COUNT | mine-tx CHAIN ADDRESS TXFILE\n";
        return 2;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}

QBTC_ENTRY(qbtc_entry)
