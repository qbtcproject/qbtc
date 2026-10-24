#include "qbtc/consensus.hpp"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
using namespace qbtc;
namespace {
int checks = 0;
void check(bool result, const char *name) {
    if (!result)
        throw std::runtime_error(name);
    checks++;
}
void rejects(const std::function<void()> &f, const char *name) {
    try {
        f();
    } catch (const Invalid &) {
        checks++;
        return;
    }
    throw std::runtime_error(name);
}
Block next(Chain &c, std::uint8_t owner = 1) {
    Address a{};
    a[0] = owner;
    auto b = candidate(c, a, c.tip().header.timestamp + 600);
    mine(b, 100000);
    return b;
}
void remine(Block &b) {
    b.header.root = merkle(b.transactions);
    b.header.nonce = 0;
    mine(b, 100000);
}
} // namespace
int main() {
    try {
        check(subsidy(0) == 0, "genesis reward");
        check(subsidy(1) == 50 * COIN, "initial reward");
        check(subsidy(209999) == 50 * COIN, "pre-halving");
        check(subsidy(210000) == 25 * COIN, "halving");
        std::uint64_t sum = 0;
        for (std::uint64_t h = 0; h < 64 * HALVING; h++) {
            auto v = subsidy(h);
            check(v <= 50 * COIN, "subsidy range");
            sum += v;
        }
        check(sum == 2099994997690000ULL, "exact emission");
        check(total_emission() == sum, "emission independent sum");
        check(sum < MAX_MONEY, "supply ceiling");
        Block terminal;
        terminal.header.height = 64 * HALVING;
        Transaction cb;
        cb.coinbase_height = terminal.header.height;
        terminal.transactions.push_back(cb);
        check(apply_transactions(terminal, {}).empty(), "zero subsidy empty coinbase");
        check(subsidy(std::numeric_limits<std::uint64_t>::max()) == 0, "height overflow");
        for (std::uint64_t e = 1; e < 64; e++) {
            check(subsidy(e * HALVING) == ((50 * COIN) >> e), "all halvings");
            check(subsidy(e * HALVING - 1) == ((50 * COIN) >> (e - 1)), "all halving boundaries");
        }
        check(hex(hash({})) == "a69f73cca23a9ac5c8b567dc185a756e97c982164fe25859e0d1dcc1475c80a615b"
                               "2123af1f5f94c11e3e9402c3ac558f500199d95b6d3e301758586281dcd26",
              "SHA3 known answer");
        Chain c(Network::Regtest);
        check(c.utxos().empty(), "no allocations");
        auto g = genesis(Network::Regtest);
        check(encode(g) == encode(genesis(Network::Regtest)), "reproducible genesis");
        check(valid_pow(g.header), "genesis PoW");
        rejects([] { Chain x(Network::Mainnet); }, "mainnet disabled");
        rejects([] { Chain x(Network::Testnet); }, "testnet disabled");
        auto b = next(c);
        check(encode(decode_block(encode(b))) == encode(b), "block canonical roundtrip");
        check(encode(decode_transaction(encode(b.transactions[0]))) == encode(b.transactions[0]),
              "transaction canonical roundtrip");
        auto raw = encode(b);
        for (std::size_t n = 0; n < raw.size(); n++)
            rejects([&] { decode_block(std::span<const std::uint8_t>(raw).first(n)); },
                    "every truncation");
        raw.push_back(0);
        rejects([&] { decode_block(raw); }, "trailing byte");
        auto bad = b;
        bad.transactions[0].outputs[0].value++;
        remine(bad);
        rejects([&] { c.add(bad); }, "inflation");
        check(c.tip().header.height == 0 && c.utxos().empty(), "atomic rejection");
        bad = b;
        bad.transactions[0].outputs[0].value = std::numeric_limits<std::uint64_t>::max();
        remine(bad);
        rejects([&] { c.add(bad); }, "money overflow");
        bad = b;
        bad.transactions[0].outputs.push_back({MAX_MONEY, {}});
        remine(bad);
        rejects([&] { c.add(bad); }, "output sum overflow");
        bad = b;
        bad.transactions[0].coinbase_height = 2;
        remine(bad);
        rejects([&] { c.add(bad); }, "coinbase height");
        bad = b;
        bad.header.height = 2;
        mine(bad, 100000);
        rejects([&] { c.add(bad); }, "header height");
        bad = b;
        bad.header.timestamp = g.header.timestamp;
        mine(bad, 100000);
        rejects([&] { c.add(bad); }, "median time");
        bad = b;
        bad.header.target[0] = 0x7e;
        mine(bad, 100000);
        rejects([&] { c.add(bad); }, "arbitrary target");
        bad = b;
        bad.header.network = Network::Testnet;
        rejects([&] { c.add(bad); }, "cross-network");
        bad = b;
        bad.header.root[0] ^= 1;
        mine(bad, 100000);
        rejects([&] { c.add(bad); }, "wrong merkle");
        bad = b;
        bad.header.previous[0] ^= 1;
        mine(bad, 100000);
        rejects([&] { c.add(bad); }, "unknown parent");
        bad = b;
        while (valid_pow(bad.header))
            bad.header.nonce++;
        rejects([&] { c.add(bad); }, "invalid PoW");
        bad = b;
        bad.transactions.push_back(bad.transactions[0]);
        rejects([&] { merkle(bad.transactions); }, "duplicate tx merkle");
        c.add(b);
        rejects([&] { c.add(b); }, "known block");
        check(c.utxos().size() == 1, "coinbase UTXO");
        auto limit = pow_limit(Network::Regtest);
        check(retarget(limit, 0, INTERVAL * SPACING, Network::Regtest) == limit, "stable target");
        auto quarter = retarget(limit, 10, 10, Network::Regtest);
        check(quarter < limit, "difficulty increases");
        check(retarget(limit, 0, 1, Network::Regtest) == quarter, "lower clamp");
        check(retarget(limit, 0, INTERVAL * SPACING * 100, Network::Regtest) == limit,
              "upper clamp and cap");
        check(work(quarter) > work(limit), "work inverse target");
        rejects([&] { retarget(limit, 10, 9, Network::Regtest); }, "negative timespan");
        rejects([&] { work(Hash{}); }, "zero target work");
        Header h = g.header;
        h.target = {};
        check(!valid_pow(h), "zero target PoW");
        h.target.fill(255);
        check(!valid_pow(h), "target over limit");
        // An independent branch overtakes; UTXOs are switched atomically.
        Chain fork(Network::Regtest);
        auto f1 = next(fork, 2);
        fork.add(f1);
        c.add(f1);
        check(blockid(c.tip().header) == blockid(b.header), "equal work keeps first seen");
        auto f2 = next(fork, 2);
        fork.add(f2);
        c.add(f2);
        check(blockid(c.tip().header) == blockid(fork.tip().header), "greater work reorg");
        check(c.utxos().size() == 2, "reorg replaces coinbase UTXOs");
        std::uint64_t supply = 0;
        for (const auto &[id, coin] : c.utxos()) {
            (void)id;
            supply += coin.output.value;
        }
        check(supply == 100 * COIN, "reorg monetary conservation");
        check(!c.utxos().contains({txid(b.transactions[0]), 0}), "orphan reward removed");
        // Independently replay 2016 blocks to cross a real difficulty boundary.
        Chain longchain(Network::Regtest);
        for (int j = 0; j < 2016; j++) {
            auto n = next(longchain);
            longchain.add(n);
        }
        check(longchain.tip().header.height == 2016, "retarget integration height");
        check(longchain.tip().header.target < limit, "retarget integration target");
        // Transaction rejection ordering and fail-closed post-quantum backend.
        auto spendblock = next(c);
        Transaction spend;
        Bytes pk(1952, 0);
        Address addr = key_address(pk);
        Utxos coins;
        Outpoint op{};
        coins.emplace(op, Coin{{10 * COIN, addr}, 1, true});
        spend.inputs.push_back({op, pk, Bytes(3309, 0)});
        spend.outputs.push_back({9 * COIN, addr});
        spendblock.transactions.push_back(spend);
        spendblock.header.height = 100;
        spendblock.transactions[0].coinbase_height = 100;
        rejects([&] { apply_transactions(spendblock, coins); }, "immature spend");
        spendblock.header.height = 101;
        spendblock.transactions[0].coinbase_height = 101;
        rejects([&] { apply_transactions(spendblock, coins); }, "forged MLDSA rejection");
        spendblock.transactions[1].inputs.push_back(spend.inputs[0]);
        rejects([&] { apply_transactions(spendblock, coins); }, "duplicate input rejected");
        check(!verify_signature(pk, Bytes{1}, Bytes(3309, 0)), "invalid fixed-length signature");
        check(!verify_signature(Bytes{}, Bytes{}, Bytes{}), "invalid signature lengths");
        auto msg = signature_message(Network::Regtest, spend, 0, {10 * COIN, addr});
        check(msg != signature_message(Network::Testnet, spend, 0, {10 * COIN, addr}),
              "signature network domain");
        check(msg != signature_message(Network::Regtest, spend, 0, {9 * COIN, addr}),
              "signature previous value binding");
        auto path = (std::filesystem::temp_directory_path() / "qbtc-consensus-test.db").string();
        c.save(path);
        auto restored = Chain::load(Network::Regtest, path);
        check(blockid(restored.tip().header) == blockid(c.tip().header), "restart tip");
        check(restored.utxos().size() == c.utxos().size(), "restart UTXOs");
        {
            std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
            f.seekp(20);
            f.put('\x13');
        }
        rejects([&] { Chain::load(Network::Regtest, path); }, "disk corruption");
        std::filesystem::remove(path);
        std::mt19937 rng(12345);
        for (int j = 0; j < 3000; j++) {
            Bytes bytes(rng() % 1024);
            for (auto &v : bytes)
                v = static_cast<std::uint8_t>(rng());
            try {
                auto decoded = decode_block(bytes);
                check(encode(decoded) == bytes, "random canonical block");
            } catch (const Invalid &) {
            }
            checks++;
        }
        std::cout << "PASS " << checks << " checks\n"
                  << "ML-DSA provider available: "
                  << (crypto_available() ? "yes" : "no; positive signature tests NOT executed")
                  << "\n"
                  << "Genesis: " << hex(blockid(g.header)) << "\n"
                  << "Emission (atomic units): " << total_emission() << "\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << " after " << checks << " checks\n";
        return 1;
    }
}
