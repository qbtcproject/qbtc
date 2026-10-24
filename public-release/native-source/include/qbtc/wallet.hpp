#pragma once
#include "qbtc/address.hpp"
#include "qbtc/consensus.hpp"
#include <memory>
#include <openssl/evp.h>

namespace qbtc::wallet {
// Private keys exist only in this wallet module, never in consensus or node APIs.
class Key {
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key_;
    explicit Key(EVP_PKEY *key);

  public:
    Key(Key &&) noexcept = default;
    Key &operator=(Key &&) noexcept = default;
    Key(const Key &) = delete;
    Key &operator=(const Key &) = delete;
    static Key generate();
    static Key load(const std::string &path, std::string_view password);
    void save(const std::string &path, std::string_view password) const;
    Bytes public_key() const;
    Address owner() const;
    Bytes sign(std::span<const std::uint8_t> message) const;
};
struct Balance {
    std::uint64_t total = 0, spendable = 0;
};
Balance balance(const Utxos &utxos, Address owner, std::uint64_t spend_height);
Transaction payment(const Key &key, const Chain &chain, Address recipient, std::uint64_t amount,
                    std::uint64_t fee, bool enforce_relay_policy = false);
std::string read_password(const char *prompt);
// New-file-only writes; POSIX 0600 / restricted Windows DACL, never overwrite keys.
void write_private_new(const std::string &path, std::span<const std::uint8_t> bytes);
Bytes read_bounded(const std::string &path, std::size_t maximum);
} // namespace qbtc::wallet
