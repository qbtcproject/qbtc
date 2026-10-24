#include "qbtc/wallet.hpp"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <openssl/rand.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif
using namespace qbtc;
namespace {
int checks = 0;
void check(bool ok, const char *label) {
    if (!ok)
        throw std::runtime_error(label);
    checks++;
}
void rejects(const std::function<void()> &f, const char *expected) {
    try {
        f();
    } catch (const Invalid &e) {
        check(std::string(e.what()).find(expected) != std::string::npos, e.what());
        return;
    }
    throw std::runtime_error(expected);
}
void extend(Chain &c, Address owner, unsigned n) {
    for (unsigned j = 0; j < n; j++) {
        auto b = candidate(c, owner, c.tip().header.timestamp + SPACING);
        mine(b, 100000);
        c.add(b);
    }
}
std::uint64_t value(const Utxos &coins) {
    std::uint64_t sum = 0;
    for (const auto &[id, c] : coins) {
        (void)id;
        sum += c.output.value;
    }
    return sum;
}
} // namespace
int main() {
    std::filesystem::path directory;
    try {
        check(crypto_available(), "required ML-DSA provider");
        auto alice = wallet::Key::generate(), bob = wallet::Key::generate();
        auto a = alice.owner(), b = bob.owner();
        check(a != b, "distinct generated keys");
        auto pk = alice.public_key();
        Bytes message = {'Q', 'B', 'T', 'C', ' ', 's', 'i', 'g', 'n', 'a', 't', 'u', 'r', 'e'};
        auto signature = alice.sign(message);
        check(verify_signature(pk, message, signature), "valid ML-DSA signature");
        auto mutated = signature;
        mutated[10] ^= 1;
        check(!verify_signature(pk, message, mutated), "mutated signature");
        message[0] ^= 1;
        check(!verify_signature(pk, message, signature), "mutated message");
        message[0] ^= 1;
        check(!verify_signature(bob.public_key(), message, signature), "wrong key");
        auto text = wallet::address(a);
        check(text.size() == 63 && wallet::parse_address(text) == a, "address roundtrip");
        for (std::size_t i = 0; i < text.size(); i++) {
            auto bad = text;
            bad[i] = bad[i] == 'q' ? 'p' : 'q';
            rejects([&] { wallet::parse_address(bad); }, "address");
        }
        auto upper = text;
        upper[0] = 'Q';
        rejects([&] { wallet::parse_address(upper); }, "address");
        rejects([&] { wallet::parse_address(text, Network::Mainnet); }, "regtest");
        // Sample the full commitment shape, including high bits and zero padding.
        for (unsigned i = 0; i < 1000; i++) {
            Address owner{};
            check(RAND_bytes(owner.data(), 32) == 1, "random address input");
            check(wallet::parse_address(wallet::address(owner)) == owner,
                  "random address roundtrip");
        }
        Bytes random(12);
        check(RAND_bytes(random.data(), 12) == 1, "temporary directory randomness");
        directory = std::filesystem::temp_directory_path() / ("qbtc-wallet-test-" + hex(random));
        std::filesystem::create_directory(directory);
        auto path = (directory / "alice.wallet").string();
        const std::string password = "Test-only strong passphrase 2026!";
        alice.save(path, password);
        check(std::filesystem::file_size(path) == 85, "encrypted wallet format");
#ifndef _WIN32
        struct stat st{};
        check(::stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600,
              "wallet file mode 0600");
#endif
        auto recovered = wallet::Key::load(path, password);
        check(recovered.public_key() == pk, "seed recovery identical public key");
        check(verify_signature(pk, message, recovered.sign(message)), "recovered key signs");
        rejects([&] { wallet::Key::load(path, "wrong password"); }, "wrong password");
        rejects([&] { alice.save(path, password); }, "never overwritten");
        rejects([&] { alice.save((directory / "weak.wallet").string(), "short"); }, "12 bytes");
        auto backup = (directory / "backup.wallet").string();
        recovered.save(backup, password);
        auto backup_key = wallet::Key::load(backup, password);
        check(backup_key.owner() == a, "encrypted backup recovery");
        check(wallet::read_bounded(path, 85) != wallet::read_bounded(backup, 85),
              "fresh salt and nonce per backup");
        auto corrupted = wallet::read_bounded(path, 85);
        corrupted[40] ^= 1;
        auto damaged = (directory / "damaged.wallet").string();
        wallet::write_private_new(damaged, corrupted);
        rejects([&] { wallet::Key::load(damaged, password); }, "wrong password");
        // Build actual coinbase UTXOs owned by Alice, then a valid signed payment.
        Chain chain(Network::Regtest);
        extend(chain, a, 100);
        auto bal = wallet::balance(chain.utxos(), a, 101);
        check(bal.total == 5000 * COIN && bal.spendable == 50 * COIN,
              "coinbase balance and maturity");
        rejects([&] { wallet::payment(alice, chain, b, 51 * COIN, 1000); }, "insufficient");
        auto tx = wallet::payment(alice, chain, b, 10 * COIN, 1000);
        check(tx.inputs.size() == 1 && tx.outputs.size() == 2, "payment with change");
        auto valid = candidate(chain, a, chain.tip().header.timestamp + SPACING, {tx});
        check(valid.transactions[0].outputs[0].value == 50 * COIN + 1000, "fees paid to miner");
        auto badtx = tx;
        badtx.outputs[0].value++;
        rejects([&] { candidate(chain, a, valid.header.timestamp, {badtx}); }, "signature");
        auto duplicate = tx;
        duplicate.inputs.push_back(duplicate.inputs[0]);
        rejects([&] { candidate(chain, a, valid.header.timestamp, {duplicate}); }, "double spend");
        auto conflict = wallet::payment(alice, chain, b, 11 * COIN, 1000);
        rejects([&] { candidate(chain, a, valid.header.timestamp, {tx, conflict}); },
                "missing UTXO");
        auto forged = tx;
        forged.inputs[0].signature[20] ^= 1;
        rejects([&] { candidate(chain, a, valid.header.timestamp, {forged}); }, "signature");
        // A valid signature over an inflationary transaction must still be rejected.
        auto inflation = tx;
        inflation.outputs[0].value = 51 * COIN;
        inflation.outputs.resize(1);
        auto prev = chain.utxos().at(inflation.inputs[0].previous).output;
        inflation.inputs[0].signature =
            alice.sign(signature_message(Network::Regtest, inflation, 0, prev));
        rejects([&] { candidate(chain, a, valid.header.timestamp, {inflation}); }, "overspend");
        auto underpay = valid;
        underpay.transactions[0].outputs[0].value++;
        underpay.header.root = merkle(underpay.transactions);
        mine(underpay, 100000);
        rejects([&] { chain.add(underpay); }, "excess coinbase");
        check(chain.tip().header.height == 100, "invalid payment blocks do not mutate chain");
        mine(valid, 100000);
        chain.add(valid);
        check(wallet::balance(chain.utxos(), b, 102).total == 10 * COIN, "recipient credited");
        check(value(chain.utxos()) == 5050 * COIN, "fee is not new issuance");
        rejects([&] { candidate(chain, a, valid.header.timestamp + SPACING, {tx}); },
                "missing UTXO");
        auto database = (directory / "chain.db").string();
        chain.save(database);
        auto restored = Chain::load(Network::Regtest, database);
        check(value(restored.utxos()) == value(chain.utxos()), "signed chain restart");
        check(wallet::balance(restored.utxos(), backup_key.owner(), 102).total ==
                  wallet::balance(chain.utxos(), a, 102).total,
              "recovered wallet restores funds");
        // Reorg from height 100 removes payment, returns spent input, conserves issuance.
        Chain fork(Network::Regtest);
        auto cursor = valid.header.previous;
        std::vector<Block> history;
        while (chain.block(cursor).header.height) {
            history.push_back(chain.block(cursor));
            cursor = chain.block(cursor).header.previous;
        }
        for (auto it = history.rbegin(); it != history.rend(); ++it)
            fork.add(*it);
        extend(fork, b, 1);
        auto f1 = fork.tip();
        chain.add(f1);
        extend(fork, b, 1);
        chain.add(fork.tip());
        check(wallet::balance(chain.utxos(), b, 103).total == 100 * COIN,
              "reorg removes orphan payment");
        check(chain.utxos().contains(tx.inputs[0].previous), "reorg restores spent input");
        check(value(chain.utxos()) == 5100 * COIN, "reorg with signatures preserves supply");
        // A multi-input payment binds every input. This also tests signature placeholder
        // canonicalization after some signatures have already been filled in.
        extend(chain, a, 100);
        auto multi = wallet::payment(alice, chain, b, 75 * COIN, 1234);
        check(multi.inputs.size() >= 2, "multi-input selection");
        auto multi_block = candidate(chain, a, chain.tip().header.timestamp + SPACING, {multi});
        mine(multi_block, 100000);
        chain.add(multi_block);
        check(value(chain.utxos()) == subsidy(1) * chain.tip().header.height,
              "multi-input monetary conservation");
        // Parent/child spends within one block must collapse transient outputs in
        // the reversible delta, then restore the original funding coin on reorg.
        Chain nested(Network::Regtest);
        extend(nested, a, 100);
        auto parent = wallet::payment(alice, nested, b, 10 * COIN, 1000);
        Transaction child;
        child.inputs.push_back({{txid(parent), 0}, bob.public_key(), {}});
        child.outputs.push_back({9 * COIN, a});
        child.outputs.push_back({COIN - 2000, b});
        child.inputs[0].signature =
            bob.sign(signature_message(Network::Regtest, child, 0, parent.outputs[0]));
        auto funding = parent.inputs[0].previous;
        auto package =
            candidate(nested, a, nested.tip().header.timestamp + SPACING, {parent, child});
        mine(package, 100000);
        nested.add(package);
        check(!nested.utxos().contains({txid(parent), 0}), "same-block transient output consumed");
        check(value(nested.utxos()) == 5050 * COIN, "same-block package fees not issuance");
        Chain alternative(Network::Regtest);
        auto id = package.header.previous;
        std::vector<Block> prefix;
        while (nested.block(id).header.height) {
            prefix.push_back(nested.block(id));
            id = nested.block(id).header.previous;
        }
        for (auto it = prefix.rbegin(); it != prefix.rend(); ++it)
            alternative.add(*it);
        extend(alternative, b, 1);
        nested.add(alternative.tip());
        extend(alternative, b, 1);
        nested.add(alternative.tip());
        check(nested.utxos().contains(funding) && !nested.utxos().contains({txid(child), 0}),
              "same-block package undo restores funding only");
        check(nested.utxos() == alternative.utxos(), "same-block package reorg exact state");
        std::filesystem::remove_all(directory);
        std::cout << "PASS wallet/ML-DSA " << checks
                  << " checks (positive signatures, encrypted recovery, fees, double spends, "
                     "signed reorg)\n";
        return 0;
    } catch (const std::exception &e) {
        if (!directory.empty())
            std::filesystem::remove_all(directory);
        std::cerr << "FAIL wallet: " << e.what() << " after " << checks << " checks\n";
        return 1;
    }
}
