#include "qbtc/mempool.hpp"
#include "qbtc/storage.hpp"
#include <algorithm>
#include <set>
namespace qbtc::mempool {
namespace {
void need(bool ok, const char *message) {
    if (!ok)
        throw Invalid(message);
}
void put32(Bytes &bytes, std::uint32_t value) {
    for (unsigned j = 0; j < 4; j++)
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * j)));
}
struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t pos = 0;
    auto take(std::size_t size) {
        need(size <= bytes.size() - pos, "truncated mempool file");
        auto result = bytes.subspan(pos, size);
        pos += size;
        return result;
    }
    std::uint32_t u32() {
        auto raw = take(4);
        std::uint32_t result = 0;
        for (unsigned j = 0; j < 4; j++)
            result |= std::uint32_t(raw[j]) << (8 * j);
        return result;
    }
};
} // namespace
void check_admission(const Transaction &tx) {
    check_relay_shape(tx);
}
Bytes encode_file(Network network, const std::vector<Transaction> &transactions) {
    need(transactions.size() <= MAX_TRANSACTIONS, "mempool file transaction count");
    Bytes bytes{'Q', 'B', 'M', 'P', 1};
    put32(bytes, static_cast<std::uint32_t>(network));
    auto id = blockid(genesis(network).header);
    bytes.insert(bytes.end(), id.begin(), id.end());
    put32(bytes, static_cast<std::uint32_t>(transactions.size()));
    std::size_t total = 0;
    std::set<Hash> ids;
    for (const auto &tx : transactions) {
        need(tx.coinbase_height == 0 && !tx.inputs.empty(), "mempool file normal transaction");
        need(ids.insert(txid(tx)).second, "duplicate mempool file transaction");
        auto raw = encode(tx);
        need(raw.size() <= MAX_BLOCK_BYTES - total, "mempool file byte budget");
        total += raw.size();
        put32(bytes, static_cast<std::uint32_t>(raw.size()));
        bytes.insert(bytes.end(), raw.begin(), raw.end());
    }
    auto digest = hash(bytes);
    bytes.insert(bytes.end(), digest.begin(), digest.end());
    return bytes;
}
std::vector<Transaction> decode_file(Network network, std::span<const std::uint8_t> bytes) {
    need(bytes.size() >= 141 && bytes.size() <= MAX_FILE_BYTES, "mempool file size");
    auto content = bytes.first(bytes.size() - 64);
    auto digest = hash(content);
    need(std::equal(digest.begin(), digest.end(), bytes.end() - 64), "mempool file checksum");
    Reader reader{content};
    constexpr std::array<std::uint8_t, 5> magic{'Q', 'B', 'M', 'P', 1};
    auto actual = reader.take(5);
    need(std::equal(magic.begin(), magic.end(), actual.begin()), "mempool file format");
    need(reader.u32() == static_cast<std::uint32_t>(network), "mempool file network");
    auto expected = blockid(genesis(network).header);
    actual = reader.take(64);
    need(std::equal(expected.begin(), expected.end(), actual.begin()), "mempool file genesis");
    auto count = reader.u32();
    need(count <= MAX_TRANSACTIONS, "mempool file transaction count");
    std::vector<Transaction> result;
    result.reserve(count);
    std::size_t total = 0;
    std::set<Hash> ids;
    for (std::uint32_t j = 0; j < count; j++) {
        auto size = reader.u32();
        need(size <= MAX_BLOCK_BYTES - total, "mempool file byte budget");
        total += size;
        auto tx = decode_transaction(reader.take(size));
        need(tx.coinbase_height == 0 && !tx.inputs.empty(), "mempool file normal transaction");
        need(ids.insert(txid(tx)).second, "duplicate mempool file transaction");
        result.push_back(std::move(tx));
    }
    need(reader.pos == content.size(), "mempool file trailing bytes");
    return result;
}
void save(const std::string &path, Network network, const std::vector<Transaction> &transactions) {
    auto bytes = encode_file(network, transactions);
    storage::replace_atomic(path, bytes);
}
std::vector<Transaction> load(const std::string &path, Network network) {
    auto bytes = storage::read_bounded(path, MAX_FILE_BYTES);
    return decode_file(network, bytes);
}
} // namespace qbtc::mempool
