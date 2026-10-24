#pragma once
#include "qbtc/consensus.hpp"
namespace qbtc::mempool {
// Local policy, not consensus. File contains signed public transactions only.
inline constexpr std::size_t MAX_TRANSACTIONS = 64;
inline constexpr std::size_t MAX_FILE_BYTES = MAX_BLOCK_BYTES + 1024;
// Relay policy limits expensive validation without changing block consensus.
inline constexpr std::size_t MAX_RELAY_BYTES = 64 * 1024;
inline constexpr std::size_t MAX_RELAY_INPUTS = 32;
inline constexpr std::size_t MAX_RELAY_OUTPUTS = 128;
// Shared policy preflight for signed submissions and unsigned wallet templates.
inline void check_relay_shape(const Transaction &tx, bool unsigned_template = false) {
    if (tx.version != 1 || tx.coinbase_height != 0 || tx.inputs.empty() || tx.outputs.empty())
        throw Invalid("relay requires a normal transaction");
    if (tx.inputs.size() > MAX_RELAY_INPUTS || tx.outputs.size() > MAX_RELAY_OUTPUTS)
        throw Invalid("relay transaction item policy");
    std::size_t bytes = 20 + tx.outputs.size() * 40;
    for (const auto &input : tx.inputs) {
        if (input.public_key.size() != 1952 || input.signature.size() != (unsigned_template ? 0u : 3309u))
            throw Invalid("relay signature or public key length");
        bytes += 76 + 1952 + 3309;
        if (bytes > MAX_RELAY_BYTES)
            throw Invalid("payment exceeds relay size policy; use a smaller payment or consolidate inputs");
    }
}
void check_admission(const Transaction &transaction);
Bytes encode_file(Network network, const std::vector<Transaction> &transactions);
std::vector<Transaction> decode_file(Network network, std::span<const std::uint8_t> bytes);
void save(const std::string &path, Network network, const std::vector<Transaction> &transactions);
std::vector<Transaction> load(const std::string &path, Network network);
} // namespace qbtc::mempool
