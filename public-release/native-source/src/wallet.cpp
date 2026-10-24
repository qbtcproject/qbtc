#include "qbtc/wallet.hpp"
#include "qbtc/mempool.hpp"
#include "qbtc/storage.hpp"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/params.h>
#include <openssl/rand.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace qbtc::wallet {
namespace {
void need(bool ok, const char *msg) {
    if (!ok)
        throw Invalid(msg);
}
struct Secret {
    Bytes bytes;
    explicit Secret(std::size_t n) : bytes(n) {}
    ~Secret() {
        OPENSSL_cleanse(bytes.data(), bytes.size());
    }
    Secret(const Secret &) = delete;
    Secret &operator=(const Secret &) = delete;
};
using Pctx = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using Mctx = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using Cctx = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
constexpr std::uint64_t KDF_N = 131072;
Bytes wallet_header() {
    Bytes header{'Q','W','L','T',1};
    auto network = static_cast<std::uint32_t>(ACTIVE_NETWORK);
    for (unsigned j=0;j<4;++j) header.push_back(static_cast<std::uint8_t>(network >> (8*j)));
    return header;
}
void derive_into(Secret &output, std::string_view password, std::span<const std::uint8_t> salt) {
    need(!password.empty() && password.size() <= 1024, "password length must be 1..1024 bytes");
    need(EVP_PBE_scrypt(password.data(), password.size(), salt.data(), salt.size(), KDF_N, 8, 1,
                        256ULL * 1024 * 1024, output.bytes.data(), output.bytes.size()) == 1,
         "scrypt failure");
}
} // namespace
Key::Key(EVP_PKEY *key) : key_(key, EVP_PKEY_free) {
    need(key != nullptr, "ML-DSA-65 key unavailable");
}
Key Key::generate() {
    need(crypto_available(), "ML-DSA-65 provider required; rebuild with pinned OpenSSL");
    Pctx ctx(EVP_PKEY_CTX_new_from_name(nullptr, "ML-DSA-65", nullptr), EVP_PKEY_CTX_free);
    need(bool(ctx) && EVP_PKEY_keygen_init(ctx.get()) == 1, "keygen initialization");
    EVP_PKEY *key = nullptr;
    need(EVP_PKEY_generate(ctx.get(), &key) == 1, "ML-DSA keygen failure");
    return Key(key);
}
Bytes Key::public_key() const {
    Bytes pk(1952);
    std::size_t n = 0;
    need(EVP_PKEY_get_octet_string_param(key_.get(), OSSL_PKEY_PARAM_PUB_KEY, pk.data(), pk.size(),
                                         &n) == 1 &&
             n == pk.size(),
         "public key export");
    return pk;
}
Address Key::owner() const {
    return key_address(public_key());
}
Bytes Key::sign(std::span<const std::uint8_t> message) const {
    Mctx ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    need(bool(ctx) && EVP_DigestSignInit_ex(ctx.get(), nullptr, nullptr, nullptr, nullptr,
                                            key_.get(), nullptr) == 1,
         "ML-DSA signing init");
    Bytes signature(3309);
    std::size_t n = signature.size();
    need(EVP_DigestSign(ctx.get(), signature.data(), &n, message.data(), message.size()) == 1 &&
             n == signature.size(),
         "ML-DSA signing failure");
    return signature;
}
void Key::save(const std::string &path, std::string_view password) const {
    need(password.size() >= 12, "new wallet password requires at least 12 bytes");
    Secret seed(32), encryption_key(32);
    std::size_t n = 0;
    need(EVP_PKEY_get_octet_string_param(key_.get(), "seed", seed.bytes.data(), seed.bytes.size(),
                                         &n) == 1 &&
             n == 32,
         "seed export");
    // Fixed versioned format: 5-byte magic, regtest network u32 LE, salt16, nonce12,
    // seed ciphertext32, authentication tag16. All metadata is authenticated.
    Bytes file = wallet_header();
    Bytes salt(16), nonce(12);
    need(RAND_bytes(salt.data(), 16) == 1 && RAND_bytes(nonce.data(), 12) == 1,
         "wallet random generation");
    file.insert(file.end(), salt.begin(), salt.end());
    file.insert(file.end(), nonce.begin(), nonce.end());
    derive_into(encryption_key, password, salt);
    Cctx ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    need(bool(ctx) && EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr,
                                         encryption_key.bytes.data(), nonce.data()) == 1,
         "wallet encryption init");
    int produced = 0;
    need(EVP_EncryptUpdate(ctx.get(), nullptr, &produced, file.data(),
                           static_cast<int>(file.size())) == 1,
         "wallet authenticated header");
    Bytes cipher(48);
    int count = 0, tail = 0;
    need(EVP_EncryptUpdate(ctx.get(), cipher.data(), &count, seed.bytes.data(), 32) == 1,
         "wallet encryption");
    need(EVP_EncryptFinal_ex(ctx.get(), cipher.data() + count, &tail) == 1 && count + tail == 32,
         "wallet encryption final");
    file.insert(file.end(), cipher.begin(), cipher.begin() + 32);
    Bytes tag(16);
    need(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, 16, tag.data()) == 1,
         "wallet authentication tag");
    file.insert(file.end(), tag.begin(), tag.end());
    write_private_new(path, file);
}
Key Key::load(const std::string &path, std::string_view password) {
    auto file = read_bounded(path, 85);
    need(file.size() == 85 &&
             std::equal(file.begin(), file.begin() + 9,
                        wallet_header().begin()),
         "wallet format or network");
    Secret encryption_key(32), seed(48);
    derive_into(encryption_key, password, std::span<const std::uint8_t>(file).subspan(9, 16));
    Cctx ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    need(bool(ctx) && EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr,
                                         encryption_key.bytes.data(), file.data() + 25) == 1,
         "wallet decryption init");
    int count = 0, tail = 0;
    need(EVP_DecryptUpdate(ctx.get(), nullptr, &count, file.data(), 37) == 1,
         "wallet authenticated header");
    need(EVP_DecryptUpdate(ctx.get(), seed.bytes.data(), &count, file.data() + 37, 32) == 1,
         "wallet decryption");
    need(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, 16, file.data() + 69) == 1,
         "wallet tag load");
    need(EVP_DecryptFinal_ex(ctx.get(), seed.bytes.data() + count, &tail) == 1 &&
             count + tail == 32,
         "wrong password or damaged wallet");
    Pctx ctx_key(EVP_PKEY_CTX_new_from_name(nullptr, "ML-DSA-65", nullptr), EVP_PKEY_CTX_free);
    need(bool(ctx_key) && EVP_PKEY_fromdata_init(ctx_key.get()) == 1, "key import init");
    OSSL_PARAM params[] = {OSSL_PARAM_construct_octet_string("seed", seed.bytes.data(), 32),
                           OSSL_PARAM_construct_end()};
    EVP_PKEY *key = nullptr;
    need(EVP_PKEY_fromdata(ctx_key.get(), &key, EVP_PKEY_KEYPAIR, params) == 1,
         "seed recovery failure");
    return Key(key);
}
Balance balance(const Utxos &coins, Address owner, std::uint64_t height) {
    Balance b;
    for (const auto &[op, coin] : coins) {
        (void)op;
        if (coin.output.owner != owner)
            continue;
        need(coin.output.value <= MAX_MONEY - b.total, "balance overflow");
        b.total += coin.output.value;
        if (!coin.coinbase || (height >= coin.height && height - coin.height >= MATURITY))
            b.spendable += coin.output.value;
    }
    return b;
}
Transaction payment(const Key &key, const Chain &chain, Address recipient, std::uint64_t amount,
                    std::uint64_t fee, bool enforce_relay_policy) {
    need(amount > 0 && amount <= MAX_MONEY && fee <= MAX_MONEY - amount,
         "payment amount or fee range");
    need(chain.tip_header().height < std::numeric_limits<std::uint64_t>::max(),
         "payment height overflow");
    auto height = chain.tip_header().height + 1;
    auto owner = key.owner();
    auto pk = key.public_key();
    std::uint64_t selected = 0;
    Transaction tx;
    std::vector<Output> previous;
    for (const auto &[op, coin] : chain.utxos()) {
        if (coin.output.owner != owner ||
            (coin.coinbase && (height < coin.height || height - coin.height < MATURITY)))
            continue;
        need(tx.inputs.size() < MAX_ITEMS, "too many payment inputs");
        need(coin.output.value <= MAX_MONEY - selected, "payment sum overflow");
        selected += coin.output.value;
        tx.inputs.push_back({op, pk, {}});
        previous.push_back(coin.output);
        if (selected >= amount + fee)
            break;
    }
    need(selected >= amount + fee, "insufficient mature funds");
    tx.outputs.push_back({amount, recipient});
    if (selected > amount + fee)
        tx.outputs.push_back({selected - amount - fee, owner});
    if (enforce_relay_policy) mempool::check_relay_shape(tx, true);
    for (std::size_t i = 0; i < tx.inputs.size(); i++) {
        auto signature = key.sign(signature_message(chain.tip_header().network, tx,
                                                    static_cast<std::uint32_t>(i), previous[i]));
        tx.inputs[i].signature.swap(signature);
    }
    (void)encode(tx);
    return tx;
}
Bytes read_bounded(const std::string &path, std::size_t maximum) {
    return storage::read_bounded(path, maximum);
}
void write_private_new(const std::string &path, std::span<const std::uint8_t> bytes) {
    storage::write_new(path, bytes);
}
std::string read_password(const char *prompt) {
#ifdef _WIN32
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD previous = 0;
    need(GetConsoleMode(input, &previous) != 0, "password requires an interactive console");
    need(SetConsoleMode(input, previous & ~static_cast<DWORD>(ENABLE_ECHO_INPUT)) != 0, "disable console echo");
    struct Restore {
        HANDLE input;
        DWORD mode;
        ~Restore() {
            SetConsoleMode(input, mode);
        }
    } restore{input, previous};
#else
    need(isatty(STDIN_FILENO) == 1, "password requires an interactive terminal");
    termios previous{};
    need(tcgetattr(STDIN_FILENO, &previous) == 0, "terminal attributes");
    auto hidden = previous;
    hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
    need(tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) == 0, "disable terminal echo");
    struct Restore {
        termios mode;
        ~Restore() {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &mode);
        }
    } restore{previous};
#endif
    // Announce readiness only after echo is disabled and input flushing complete.
    std::fputs(prompt, stderr);
    std::fflush(stderr);
    std::string password;
    password.reserve(1024);
    bool complete = false;
    char character = 0;
    while (std::cin.get(character)) {
        if (character == '\n') { complete = true; break; }
        if (password.size() == 1024) {
            OPENSSL_cleanse(password.data(), password.size());
            throw Invalid("password length must be 1..1024 bytes");
        }
        password.push_back(character);
    }
    std::fputs("\n", stderr);
    if (!complete || password.empty()) {
        OPENSSL_cleanse(password.data(), password.size());
        throw Invalid("password read requires a nonempty complete line");
    }
    return password;
}
} // namespace qbtc::wallet
