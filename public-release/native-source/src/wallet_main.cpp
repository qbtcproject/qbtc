#include "qbtc/platform.hpp"
#include "qbtc/wallet.hpp"
#include "qbtc/storage.hpp"
#include <algorithm>
#include <charconv>
#include <iostream>
#include <iomanip>
#include <openssl/crypto.h>
using namespace qbtc;
namespace {
struct Password {
    std::string value;
    explicit Password(const char *prompt) : value(wallet::read_password(prompt)) {}
    ~Password() {
        OPENSSL_cleanse(value.data(), value.size());
    }
    Password(const Password &) = delete;
    Password &operator=(const Password &) = delete;
};
std::uint64_t amount(std::string_view s) {
    std::uint64_t n = 0;
    auto [p, e] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (e != std::errc{} || p != s.data() + s.size() || n > MAX_MONEY)
        throw Invalid("invalid atomic-unit amount");
    return n;
}
} // namespace
int qbtc_entry(int argc, char **argv) {
    try {
        if (!crypto_available())
            throw Invalid("ML-DSA-65 provider unavailable; rebuild with pinned OpenSSL");
        if (argc == 4 && std::string_view(argv[1]) == "balance-address") {
            // Public, read-only lookup: never open a wallet file or request a password.
            auto owner = wallet::parse_address(argv[2]);
            auto chain = Chain::load(ACTIVE_NETWORK, argv[3]);
            auto b = wallet::balance(chain.utxos(), owner, chain.tip_header().height + 1);
            std::cout << "total_atoms=" << b.total
                      << " spendable_next_block_atoms=" << b.spendable << "\n";
            return 0;
        }
        if (argc == 4 && std::string_view(argv[1]) == "payment-status") {
            auto chain = Chain::load(ACTIVE_NETWORK, argv[2]);
            auto payment = decode_transaction(storage::read_bounded(argv[3], MAX_BLOCK_BYTES));
            auto id = txid(payment);
            auto cursor = blockid(chain.tip_header());
            while (chain.header(cursor).height) {
                auto block = chain.block(cursor);
                for (const auto &transaction : block.transactions) {
                    if (txid(transaction) == id) {
                        std::cout << "confirmed height=" << block.header.height << " txid=" << hex(id) << "\n";
                        return 0;
                    }
                }
                cursor = chain.header(cursor).previous;
            }
            std::cout << "unconfirmed txid=" << hex(id) << "\n";
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "create") {
            Password pass("New wallet password (12+ bytes): "), confirm("Repeat password: ");
            if (pass.value != confirm.value)
                throw Invalid("password mismatch");
            auto key = wallet::Key::generate();
            key.save(argv[2], pass.value);
            std::cout
                << wallet::address(key.owner())
                << "\nEncrypted wallet created. Keep an offline copy and remember the password.\n";
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "address") {
            Password pass("Wallet password: ");
            auto key = wallet::Key::load(argv[2], pass.value);
            std::cout << wallet::address(key.owner()) << "\n";
            return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "backup") {
            Password pass("Wallet password: ");
            auto key = wallet::Key::load(argv[2], pass.value);
            key.save(argv[3], pass.value);
            std::cout << "Encrypted backup created; test it with the address command.\n";
            return 0;
        }
        if (argc == 4 && (std::string(argv[1]) == "balance" || std::string(argv[1]) == "history")) {
            Password pass("Wallet password: ");
            auto key = wallet::Key::load(argv[2], pass.value);
            auto chain = Chain::load(ACTIVE_NETWORK, argv[3]);
            auto owner = key.owner();
            if (std::string(argv[1]) == "balance") {
                auto b = wallet::balance(chain.utxos(), owner, chain.tip_header().height + 1);
                std::cout << "total_atoms=" << b.total
                          << " spendable_next_block_atoms=" << b.spendable << "\n";
                return 0;
            }
            std::vector<Hash> blocks;
            auto cursor = blockid(chain.tip_header());
            while (chain.header(cursor).height) {
                blocks.push_back(cursor);
                cursor = chain.header(cursor).previous;
            }
            std::reverse(blocks.begin(), blocks.end());
            std::map<Outpoint, Output> owned;
            for (const auto &block_id : blocks) {
                auto b = chain.block(block_id);
                for (const auto &t : b.transactions) {
                    std::uint64_t received = 0, spent = 0;
                    auto id = txid(t);
                    for (const auto &i : t.inputs) {
                        auto it = owned.find(i.previous);
                        if (it != owned.end()) {
                            spent += it->second.value;
                            owned.erase(it);
                        }
                    }
                    for (std::size_t j = 0; j < t.outputs.size(); j++)
                        if (t.outputs[j].owner == owner) {
                            received += t.outputs[j].value;
                            owned[{id, static_cast<std::uint32_t>(j)}] = t.outputs[j];
                        }
                    if (received || spent)
                        std::cout << "height=" << b.header.height << " tx=" << hex(id)
                                  << " received_atoms=" << received << " spent_atoms=" << spent
                                  << "\n";
                }
            }
            return 0;
        }
        if (argc == 9 && std::string(argv[1]) == "send") {
            // All amounts are integer atomic units. No password or private key in argv.
            std::string_view expected = ACTIVE_NETWORK == Network::Mainnet ? "--mainnet" : "--regtest";
            if (std::string_view(argv[8]) != expected)
                throw Invalid(std::string("explicit ") + std::string(expected) + " required");
            auto recipient = wallet::parse_address(argv[4]);
            auto value = amount(argv[5]), fee = amount(argv[6]);
            if (value == 0 || fee > MAX_MONEY - value)
                throw Invalid("payment amount or fee range");
            if (fee > value || fee > COIN) std::cerr << "WARNING: unusually high transaction fee.\n";
            std::cerr << "\nVERIFY PAYMENT IN THE SIGNER\nRecipient: " << wallet::address(recipient)
                      << "\nAmount: " << value / COIN << '.' << std::setfill('0') << std::setw(8) << value % COIN
                      << " QBTC\nFee: " << fee / COIN << '.' << std::setw(8) << fee % COIN
                      << " QBTC\nType SEND to authorize (anything else cancels): " << std::flush;
            std::string confirmation;
            char character = 0;
            bool complete = false;
            while (std::cin.get(character)) {
                if (character == '\n') { complete = true; break; }
                if (confirmation.size() == 4) break;
                confirmation.push_back(character);
            }
            if (!complete || confirmation != "SEND") throw Invalid("payment cancelled; nothing signed");
            auto chain = Chain::load(ACTIVE_NETWORK, argv[3]);
            Password pass("Wallet password: ");
            auto key = wallet::Key::load(argv[2], pass.value);
            auto tx = wallet::payment(key, chain, recipient, value, fee, true);
            auto b = candidate(chain, key.owner(), chain.tip_header().timestamp + SPACING, {tx});
            (void)b;
            wallet::write_private_new(argv[7], encode(tx));
            std::cout << "Signed transaction " << hex(txid(tx))
                      << " saved. Not broadcast or mined.\n";
            return 0;
        }
        std::cerr << "Usage: qbtc-wallet create FILE | address FILE | backup FILE NEWFILE | "
                     "balance FILE CHAIN | balance-address ADDRESS CHAIN | history FILE CHAIN | payment-status CHAIN TXFILE\n"
                  << "       qbtc-wallet send WALLET CHAIN ADDRESS AMOUNT_ATOMS FEE_ATOMS TXFILE "
                  << (ACTIVE_NETWORK == Network::Mainnet ? "--mainnet\n" : "--regtest\n");
        return 2;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}

QBTC_ENTRY(qbtc_entry)
