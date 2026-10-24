#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace qbtc {
#if defined(QBTC_FINAL_PROFILE) && !defined(QBTC_MAINNET)
#error QBTC_FINAL_PROFILE requires QBTC_MAINNET
#endif
#ifdef QBTC_FINAL_PROFILE
inline constexpr char VERSION[] = "1.0.0-rc.1";
#elif defined(QBTC_MAINNET)
inline constexpr char VERSION[] = "0.9.0-mainnet";
#else
inline constexpr char VERSION[] = "0.8.0";
#endif
using Bytes = std::vector<std::uint8_t>;
using Hash = std::array<std::uint8_t, 64>;
using Address = std::array<std::uint8_t, 32>;
inline constexpr std::uint64_t COIN = 100000000, MAX_MONEY = 21000000 * COIN;
inline constexpr std::uint64_t HALVING = 210000, SPACING = 600, INTERVAL = 2016;
inline constexpr std::uint32_t MAX_BLOCK_BYTES = 4000000, MAX_ITEMS = 10000, MATURITY = 100;
enum class Network : std::uint32_t {
    Regtest = 0x51425231,
    Testnet = 0x51425431,
#ifdef QBTC_FINAL_PROFILE
    Mainnet = 0x51424d33
#else
    Mainnet = 0x51424d32
#endif
};
#ifdef QBTC_MAINNET
inline constexpr Network ACTIVE_NETWORK = Network::Mainnet;
inline constexpr char NETWORK_NAME[] = "mainnet";
inline constexpr char PUBLIC_NETWORK_FLAG[] = "--public-network";
#else
inline constexpr Network ACTIVE_NETWORK = Network::Regtest;
inline constexpr char NETWORK_NAME[] = "regtest";
inline constexpr char PUBLIC_NETWORK_FLAG[] = "--internet-test";
#endif
struct Invalid : std::runtime_error {
    using std::runtime_error::runtime_error;
};
// Operational exhaustion is not a verdict that a block violates consensus.
struct ResourceLimit : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct ResourceBudget {
    std::size_t blocks = 250000, delta_records = 1000000, active_coins = 500000;
    std::uint64_t block_bytes = 128ULL * 1024 * 1024;
};
std::uint64_t subsidy(std::uint64_t height);
std::uint64_t total_emission();
Hash hash(std::span<const std::uint8_t> data);
std::string hex(std::span<const std::uint8_t> data);
struct Outpoint {
    Hash txid{};
    std::uint32_t index{};
    auto operator<=>(const Outpoint &) const = default;
};
struct Input {
    Outpoint previous;
    Bytes public_key, signature;
};
struct Output {
    std::uint64_t value{};
    Address owner{};
    bool operator==(const Output &) const = default;
};
struct Transaction {
    std::uint32_t version = 1;
    std::uint64_t coinbase_height = 0; // zero for normal transactions; height>0 for coinbase
    std::vector<Input> inputs;
    std::vector<Output> outputs;
};
Bytes encode(const Transaction &tx, bool signatures = true);
Transaction decode_transaction(std::span<const std::uint8_t> raw);
Hash txid(const Transaction &tx);
Hash merkle(const std::vector<Transaction> &txs);
Address key_address(std::span<const std::uint8_t> public_key);
bool crypto_available();
bool verify_signature(std::span<const std::uint8_t> public_key,
                      std::span<const std::uint8_t> message,
                      std::span<const std::uint8_t> signature);
Bytes signature_message(Network network, const Transaction &tx, std::uint32_t input,
                        const Output &previous);
struct Header {
    std::uint32_t version = 1;
    Network network = ACTIVE_NETWORK;
    std::uint64_t height = 0;
    Hash previous{}, root{}, target{};
    std::uint64_t timestamp = 0, nonce = 0;
};
struct Block {
    Header header;
    std::vector<Transaction> transactions;
};
Bytes encode(const Header &h);
Bytes encode(const Block &block);
Block decode_block(std::span<const std::uint8_t> raw);
Hash blockid(const Header &h);
Hash pow_limit(Network network);
bool valid_pow(const Header &h);
Hash retarget(const Hash &previous, std::uint64_t first, std::uint64_t last, Network network);
Bytes work(const Hash &target);
Block genesis(Network network);
struct Coin {
    Output output;
    std::uint64_t height;
    bool coinbase;
    bool operator==(const Coin &) const = default;
};
struct UtxoChange {
    Outpoint outpoint;
    std::optional<Coin> before, after;
};
using UtxoDelta = std::vector<UtxoChange>;
class CoinsView;
using Utxos = std::map<Outpoint, Coin>;
Utxos apply_transactions(const Block &b, const Utxos &old);
class Chain {
    struct Entry {
        Header header;
        std::optional<std::vector<Transaction>> body;
        std::uint64_t offset = 0;
        std::uint32_t length = 0;
        Hash body_digest{};
        UtxoDelta delta;
        Bytes cumulative_work;
    };
    std::string body_path_;
    Network network_;
    ResourceBudget budget_;
    std::size_t delta_count_ = 0;
    std::uint64_t block_bytes_ = 0;
    std::map<Hash, Entry> entries_;
    Hash tip_;
    Utxos utxos_;
    CoinsView view_at(const Hash &id) const;
    void restore_tip(const Hash &id);
    struct JournalCursor {
        std::string path;
        Hash digest{}, persisted_tip{};
        std::size_t blocks = 0;
        std::uint64_t committed_bytes = 0, observed_bytes = 0;
    };
    std::vector<Hash> arrival_;
    std::optional<JournalCursor> journal_;
    std::string snapshot_origin_;
    static Chain load_journal(Network network, const std::string &path);

  public:
    explicit Chain(Network network, ResourceBudget budget = {});
    Hash add(const Block &block);
    Block tip() const;
    const Header &tip_header() const;
    const Utxos &utxos() const;
    Block block(const Hash &id) const;
    const Header &header(const Hash &id) const;
    std::uint64_t resident_block_bytes() const;
    Hash expected_target(const Hash &parent) const;
    std::size_t delta_records() const;
    void save(const std::string &path) const;
    void append(const std::string &path);
    std::uint64_t recovered_tail_bytes() const;
    static Chain load(Network network, const std::string &path);
};
Block candidate(const Chain &chain, Address recipient, std::uint64_t timestamp,
                const std::vector<Transaction> &transactions = {});
std::uint64_t mine(Block &block, std::uint64_t max_attempts);
} // namespace qbtc
