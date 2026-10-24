#include "qbtc/platform.hpp"
#include "qbtc/consensus.hpp"
#include "qbtc/storage.hpp"
#include "qbtc/utxo.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <set>

namespace qbtc {
namespace {
void require(bool ok, const char *message) {
    if (!ok)
        throw Invalid(message);
}
struct Writer {
    Bytes data;
    std::size_t limit = MAX_BLOCK_BYTES;
    void room(std::size_t n) {
        require(n <= limit - data.size(), "encoding byte limit");
    }
    void u32(std::uint32_t n) {
        room(4);
        for (unsigned i = 0; i < 4; i++)
            data.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
    }
    void u64(std::uint64_t n) {
        room(8);
        for (unsigned i = 0; i < 8; i++)
            data.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
    }
    void raw(std::span<const std::uint8_t> b) {
        room(b.size());
        data.insert(data.end(), b.begin(), b.end());
    }
    void blob(std::span<const std::uint8_t> b) {
        require(b.size() <= MAX_BLOCK_BYTES, "blob too large");
        u32(static_cast<std::uint32_t>(b.size()));
        raw(b);
    }
};
struct Reader {
    std::span<const std::uint8_t> data;
    std::size_t pos = 0;
    std::span<const std::uint8_t> take(std::size_t n) {
        require(n <= data.size() - pos, "truncated input");
        auto r = data.subspan(pos, n);
        pos += n;
        return r;
    }
    std::uint32_t u32() {
        auto b = take(4);
        std::uint32_t n = 0;
        for (unsigned i = 0; i < 4; i++)
            n |= std::uint32_t(b[i]) << (8 * i);
        return n;
    }
    std::uint64_t u64() {
        auto b = take(8);
        std::uint64_t n = 0;
        for (unsigned i = 0; i < 8; i++)
            n |= std::uint64_t(b[i]) << (8 * i);
        return n;
    }
    template <std::size_t N> std::array<std::uint8_t, N> fixed() {
        std::array<std::uint8_t, N> a;
        auto b = take(N);
        std::copy(b.begin(), b.end(), a.begin());
        return a;
    }
    Bytes blob() {
        auto n = u32();
        require(n <= MAX_BLOCK_BYTES, "blob limit");
        auto b = take(n);
        return {b.begin(), b.end()};
    }
    void end() {
        require(pos == data.size(), "trailing data");
    }
};
using Big = std::unique_ptr<BIGNUM, decltype(&BN_free)>;
Big big(std::span<const std::uint8_t> b) {
    auto p = BN_bin2bn(b.data(), static_cast<int>(b.size()), nullptr);
    if (!p)
        throw Invalid("BN allocation");
    return {p, BN_free};
}
Big fresh() {
    auto p = BN_new();
    if (!p)
        throw Invalid("BN allocation");
    return {p, BN_free};
}
Bytes big_bytes(const BIGNUM *b, int width = 0) {
    int n = std::max(width, BN_num_bytes(b));
    Bytes r(static_cast<std::size_t>(n));
    require(BN_bn2binpad(b, r.data(), n) == n, "BN encoding");
    return r;
}
Hash as_hash(const BIGNUM *b) {
    auto bytes = big_bytes(b, 64);
    require(bytes.size() == 64, "target overflow");
    Hash h;
    std::copy(bytes.begin(), bytes.end(), h.begin());
    return h;
}
Bytes sum_work(const Bytes &a, const Bytes &b) {
    auto x = big(a), y = big(b), z = fresh();
    require(BN_add(z.get(), x.get(), y.get()) == 1, "work sum");
    return big_bytes(z.get());
}
bool greater_work(const Bytes &a, const Bytes &b) {
    auto x = big(a), y = big(b);
    return BN_cmp(x.get(), y.get()) > 0;
}
bool known(Network n) {
    return n == Network::Regtest || n == Network::Testnet || n == Network::Mainnet;
}
void operational(Network n) {
#ifdef QBTC_MAINNET
    require(n == Network::Regtest || n == Network::Mainnet, "testnet remains reserved");
#else
    require(n == Network::Regtest, "testnet and mainnet are reserved, not activated");
#endif
}
std::uint64_t add_money(std::uint64_t a, std::uint64_t b) {
    require(a <= MAX_MONEY && b <= MAX_MONEY - a, "money range");
    return a + b;
}
} // namespace
std::uint64_t subsidy(std::uint64_t h) {
    if (h == 0 || h / HALVING >= 64)
        return 0;
    return (50 * COIN) >> (h / HALVING);
}
std::uint64_t total_emission() {
    std::uint64_t n = 0;
    for (std::uint64_t era = 0; era < 64; era++) {
        auto s = (50 * COIN) >> era;
        n += s * (era == 0 ? HALVING - 1 : HALVING);
    }
    return n;
}
Hash hash(std::span<const std::uint8_t> data) {
    Hash h{};
    unsigned n = 0;
    require(EVP_Digest(data.data(), data.size(), h.data(), &n, EVP_sha3_512(), nullptr) == 1 &&
                n == 64,
            "SHA3-512 failure");
    return h;
}
std::string hex(std::span<const std::uint8_t> b) {
    std::string s;
    for (auto x : b) {
        s.push_back("0123456789abcdef"[x >> 4]);
        s.push_back("0123456789abcdef"[x & 15]);
    }
    return s;
}
Bytes encode(const Transaction &t, bool signatures) {
    require(t.inputs.size() <= MAX_ITEMS && t.outputs.size() <= MAX_ITEMS,
            "transaction item limit");
    Writer w;
    w.u32(t.version);
    w.u64(t.coinbase_height);
    w.u32(static_cast<std::uint32_t>(t.inputs.size()));
    for (const auto &i : t.inputs) {
        w.raw(i.previous.txid);
        w.u32(i.previous.index);
        w.blob(i.public_key);
        if (signatures)
            w.blob(i.signature);
        else
            w.u32(0);
    }
    w.u32(static_cast<std::uint32_t>(t.outputs.size()));
    for (const auto &o : t.outputs) {
        w.u64(o.value);
        w.raw(o.owner);
    }
    require(w.data.size() <= MAX_BLOCK_BYTES, "transaction byte limit");
    return w.data;
}
Transaction decode_transaction(std::span<const std::uint8_t> raw) {
    require(raw.size() <= MAX_BLOCK_BYTES, "transaction byte limit");
    Reader r{raw};
    Transaction t;
    t.version = r.u32();
    t.coinbase_height = r.u64();
    auto n = r.u32();
    require(n <= MAX_ITEMS, "input count");
    for (std::uint32_t j = 0; j < n; j++) {
        Input i;
        i.previous.txid = r.fixed<64>();
        i.previous.index = r.u32();
        i.public_key = r.blob();
        i.signature = r.blob();
        t.inputs.push_back(std::move(i));
    }
    n = r.u32();
    require(n <= MAX_ITEMS, "output count");
    for (std::uint32_t j = 0; j < n; j++) {
        Output o;
        o.value = r.u64();
        o.owner = r.fixed<32>();
        t.outputs.push_back(o);
    }
    r.end();
    return t;
}
Hash txid(const Transaction &t) {
    Writer w;
    w.limit = MAX_BLOCK_BYTES + 32;
    w.raw(Bytes{'Q', 'B', 'T', 'C', '/', 't', 'x', '/', '1'});
    w.raw(encode(t));
    return hash(w.data);
}
Hash merkle(const std::vector<Transaction> &ts) {
    if (ts.empty())
        return hash(Bytes{'Q', 'B', 'T', 'C', '/', 'e', 'm', 'p', 't', 'y', '/', '1'});
    std::vector<Hash> level;
    std::set<Hash> ids;
    for (const auto &t : ts) {
        auto id = txid(t);
        require(ids.insert(id).second, "duplicate transaction");
        level.push_back(id);
    }
    while (level.size() > 1) {
        std::vector<Hash> next;
        for (std::size_t i = 0; i < level.size(); i += 2) {
            Writer w;
            w.raw(Bytes{'Q', 'B', 'T', 'C', '/', 'n', 'o', 'd', 'e', '/', '1'});
            w.raw(level[i]);
            w.raw(level[std::min(i + 1, level.size() - 1)]);
            next.push_back(hash(w.data));
        }
        level = std::move(next);
    }
    return level[0];
}
Address key_address(std::span<const std::uint8_t> pk) {
    require(pk.size() == 1952, "ML-DSA-65 public key length");
    Writer w;
    w.raw(Bytes{'Q', 'B', 'T', 'C', '/', 'k', 'e', 'y', '/', '1'});
    w.raw(pk);
    Address a;
    unsigned n = 0;
    require(EVP_Digest(w.data.data(), w.data.size(), a.data(), &n, EVP_sha3_256(), nullptr) == 1 &&
                n == 32,
            "SHA3-256 failure");
    return a;
}
bool crypto_available() {
    auto *p = EVP_PKEY_CTX_new_from_name(nullptr, "ML-DSA-65", nullptr);
    if (!p)
        return false;
    EVP_PKEY_CTX_free(p);
    return true;
}
bool verify_signature(std::span<const std::uint8_t> pk, std::span<const std::uint8_t> message,
                      std::span<const std::uint8_t> sig) {
    if (pk.size() != 1952 || sig.size() != 3309)
        return false;
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> import(
        EVP_PKEY_CTX_new_from_name(nullptr, "ML-DSA-65", nullptr), EVP_PKEY_CTX_free);
    if (!import || EVP_PKEY_fromdata_init(import.get()) != 1)
        return false;
    OSSL_PARAM parameters[] = {
        OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY,
                                          const_cast<std::uint8_t *>(pk.data()), pk.size()),
        OSSL_PARAM_construct_end()};
    EVP_PKEY *raw_key = nullptr;
    if (EVP_PKEY_fromdata(import.get(), &raw_key, EVP_PKEY_PUBLIC_KEY, parameters) != 1)
        return false;
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw_key, EVP_PKEY_free);
    if (!key)
        return false;
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx)
        return false;
    if (EVP_DigestVerifyInit_ex(ctx.get(), nullptr, nullptr, nullptr, nullptr, key.get(),
                                nullptr) != 1)
        return false;
    return EVP_DigestVerify(ctx.get(), sig.data(), sig.size(), message.data(), message.size()) == 1;
}
Bytes signature_message(Network n, const Transaction &t, std::uint32_t index, const Output &prev) {
    Writer w;
    w.limit = MAX_BLOCK_BYTES + 128;
    w.raw(Bytes{'Q', 'B', 'T', 'C', '/', 's', 'i', 'g', '/', '1'});
    w.u32(static_cast<std::uint32_t>(n));
    w.raw(encode(t, false));
    w.u32(index);
    w.u64(prev.value);
    w.raw(prev.owner);
    return w.data;
}
Bytes encode(const Header &h) {
    Writer w;
    w.u32(h.version);
    w.u32(static_cast<std::uint32_t>(h.network));
    w.u64(h.height);
    w.raw(h.previous);
    w.raw(h.root);
    w.raw(h.target);
    w.u64(h.timestamp);
    w.u64(h.nonce);
    return w.data;
}
Bytes encode(const Block &b) {
    require(b.transactions.size() <= MAX_ITEMS, "block transaction count");
    Writer w;
    w.raw(encode(b.header));
    w.u32(static_cast<std::uint32_t>(b.transactions.size()));
    for (const auto &t : b.transactions)
        w.blob(encode(t));
    require(w.data.size() <= MAX_BLOCK_BYTES, "block byte limit");
    return w.data;
}
Block decode_block(std::span<const std::uint8_t> raw) {
    require(raw.size() <= MAX_BLOCK_BYTES, "block byte limit");
    Reader r{raw};
    Block b;
    auto &h = b.header;
    h.version = r.u32();
    h.network = static_cast<Network>(r.u32());
    require(known(h.network), "unknown network");
    h.height = r.u64();
    h.previous = r.fixed<64>();
    h.root = r.fixed<64>();
    h.target = r.fixed<64>();
    h.timestamp = r.u64();
    h.nonce = r.u64();
    auto n = r.u32();
    require(n <= MAX_ITEMS, "block transaction count");
    for (std::uint32_t j = 0; j < n; j++)
        b.transactions.push_back(decode_transaction(r.blob()));
    r.end();
    return b;
}
Hash blockid(const Header &h) {
    Writer w;
    w.raw(Bytes{'Q', 'B', 'T', 'C', '/', 'p', 'o', 'w', '/', '1'});
    w.raw(encode(h));
    return hash(w.data);
}
Hash pow_limit(Network n) {
    require(known(n), "unknown network");
    Hash t;
    t.fill(255);
    if (n == Network::Regtest)
        t[0] = 0x7f;
    else {
        t[0] = 0;
        t[1] = 0;
        t[2] = 0x0f;
    }
    return t;
}
bool valid_pow(const Header &h) {
    Hash zero{};
    return known(h.network) && h.target != zero && h.target <= pow_limit(h.network) &&
           blockid(h) <= h.target;
}
Hash retarget(const Hash &prev, std::uint64_t first, std::uint64_t last, Network n) {
    require(prev != Hash{} && prev <= pow_limit(n), "invalid previous target");
    require(last >= first, "negative retarget duration");
    constexpr std::uint64_t period = INTERVAL * SPACING;
    auto elapsed = std::clamp(last - first, period / 4, period * 4);
    auto t = big(prev);
    require(BN_mul_word(t.get(), static_cast<BN_ULONG>(elapsed)) == 1, "target multiply");
    require(BN_div_word(t.get(), static_cast<BN_ULONG>(period)) != static_cast<BN_ULONG>(-1),
            "target divide");
    auto limit = big(pow_limit(n));
    if (BN_cmp(t.get(), limit.get()) > 0)
        return pow_limit(n);
    if (BN_is_zero(t.get()))
        BN_one(t.get());
    return as_hash(t.get());
}
Bytes work(const Hash &target) {
    require(target != Hash{}, "zero target");
    auto denominator = big(target), numerator = fresh(), result = fresh();
    require(BN_add_word(denominator.get(), 1) == 1 && BN_set_bit(numerator.get(), 512) == 1,
            "work operands");
    std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)> ctx(BN_CTX_new(), BN_CTX_free);
    require(bool(ctx) &&
                BN_div(result.get(), nullptr, numerator.get(), denominator.get(), ctx.get()) == 1,
            "work divide");
    return big_bytes(result.get());
}
Block genesis(Network n) {
    operational(n);
    Block b;
    b.header.network = n;
    b.header.timestamp = 1792195200;
    b.header.target = pow_limit(n);
    b.header.root = merkle({});
#ifdef QBTC_MAINNET
    if (n == Network::Mainnet) {
#ifdef QBTC_FINAL_PROFILE
        // Genesis commits this name in a domain-separated, non-transaction root.
        // It contains zero transactions, allocates nothing and is a frozen origin exception.
        b.header.root = hash(Bytes{81,66,84,67,47,103,101,110,101,115,105,115,47,49,58,102,117,99,107,103,114,101,101,100});
        b.header.target = Hash{0x00, 0x00, 0x00, 0x03, 0x88, 0x80, 0x4c, 0xf9, 0x95, 0x2c, 0xfc, 0x71, 0x36, 0x52, 0xbd, 0x74, 0xbe, 0x16, 0x03, 0x9e, 0x3d, 0x1a, 0x53, 0x28, 0xb1, 0xc4, 0x36, 0x52, 0xd7, 0xf4, 0x80, 0x57, 0x53, 0x7d, 0x0e, 0x7f, 0xa4, 0x40, 0x1e, 0x51, 0x21, 0xe4, 0x7d, 0x0f, 0xa3, 0x21, 0x98, 0xec, 0xbf, 0xb4, 0xe3, 0xd7, 0x8b, 0xa4, 0xcb, 0xa7, 0x0c, 0x01, 0xcd, 0x0e, 0x56, 0x3c, 0xa0, 0x97};
        b.header.timestamp = 1792627200ULL;
        b.header.nonce = 635623644ULL;
        require(valid_pow(b.header) && hex(blockid(b.header)) ==
                "0000000098640dbb2dfc0f2daad73b786e7f0c823ace8c9f160688dd624032354fa284ef727ec32e9e6b46a6364deb403407a22f9fa63aeca778b33a609e17fc", "final genesis identity mismatch");
        return b;
#else
        b.header.timestamp = 1792627200ULL;
        b.header.nonce = 509501ULL;
        require(valid_pow(b.header) && hex(blockid(b.header)) ==
                "000006d71412362513fb97c48c0a1371b0edd3e7e6a29e91a8318f889181e5dd1255621ebcc717083a1a135c57c12b6c3885e2e0dbfac410d43089fbb49ed73c", "mainnet genesis identity mismatch");
        return b;
#endif
    }
#endif
    while (!valid_pow(b.header))
        b.header.nonce++;
    return b;
}
Chain::Chain(Network n, ResourceBudget budget) : network_(n), budget_(budget) {
    operational(n);
    auto g = genesis(n);
    tip_ = blockid(g.header);
    entries_.emplace(tip_, Entry{g.header, g.transactions, 0, 0, {}, {}, work(g.header.target)});
}
const Header &Chain::tip_header() const {
    return entries_.at(tip_).header;
}
Block Chain::tip() const {
    return block(tip_);
}
const Utxos &Chain::utxos() const {
    return utxos_;
}
const Header &Chain::header(const Hash &id) const {
    auto it = entries_.find(id);
    require(it != entries_.end(), "unknown block");
    return it->second.header;
}
std::uint64_t Chain::resident_block_bytes() const {
    return block_bytes_;
}
Block Chain::block(const Hash &id) const {
    auto it = entries_.find(id);
    require(it != entries_.end(), "unknown block");
    const auto &entry = it->second;
    if (entry.body)
        return Block{entry.header, *entry.body};
    std::ifstream file(file_path(body_path_), std::ios::binary);
    require(bool(file), "block body store open");
    file.seekg(static_cast<std::streamoff>(entry.offset));
    Bytes raw(entry.length);
    file.read(reinterpret_cast<char *>(raw.data()), static_cast<std::streamsize>(raw.size()));
    require(bool(file), "block body store read");
    require(hash(raw) == entry.body_digest, "block body store checksum");
    auto result = decode_block(raw);
    require(blockid(result.header) == id, "block body store identity");
    return result;
}
CoinsView Chain::view_at(const Hash &id) const {
    require(entries_.contains(id), "unknown branch parent");
    CoinsView view(utxos_);
    auto left = tip_, right = id;
    std::vector<Hash> detach, attach;
    while (entries_.at(left).header.height > entries_.at(right).header.height) {
        detach.push_back(left);
        left = entries_.at(left).header.previous;
    }
    while (entries_.at(right).header.height > entries_.at(left).header.height) {
        attach.push_back(right);
        right = entries_.at(right).header.previous;
    }
    while (left != right) {
        detach.push_back(left);
        attach.push_back(right);
        left = entries_.at(left).header.previous;
        right = entries_.at(right).header.previous;
    }
    for (const auto &block : detach)
        view.apply(entries_.at(block).delta, false);
    for (auto it = attach.rbegin(); it != attach.rend(); ++it)
        view.apply(entries_.at(*it).delta, true);
    return view;
}
void Chain::restore_tip(const Hash &id) {
    auto entry = entries_.find(id);
    require(entry != entries_.end() &&
                !greater_work(entries_.at(tip_).cumulative_work, entry->second.cumulative_work),
            "snapshot tip work");
    auto view = view_at(id);
    auto delta = view.delta();
    auto prepared = prepare_commit(delta);
    commit_prepared(utxos_, delta, prepared);
    tip_ = id;
}
std::size_t Chain::delta_records() const {
    return delta_count_;
}
Hash Chain::expected_target(const Hash &parent) const {
    const auto &p = entries_.at(parent).header;
    require(p.height < std::numeric_limits<std::uint64_t>::max(), "height overflow");
    auto next = p.height + 1;
    // Experimental reset v2: genesis carries no reward; height one starts calibrated PoW.
    if (network_ == Network::Mainnet && next == 1)
#ifdef QBTC_FINAL_PROFILE
        return Hash{0x00, 0x00, 0x00, 0x03, 0x88, 0x80, 0x4c, 0xf9, 0x95, 0x2c, 0xfc, 0x71, 0x36, 0x52, 0xbd, 0x74, 0xbe, 0x16, 0x03, 0x9e, 0x3d, 0x1a, 0x53, 0x28, 0xb1, 0xc4, 0x36, 0x52, 0xd7, 0xf4, 0x80, 0x57, 0x53, 0x7d, 0x0e, 0x7f, 0xa4, 0x40, 0x1e, 0x51, 0x21, 0xe4, 0x7d, 0x0f, 0xa3, 0x21, 0x98, 0xec, 0xbf, 0xb4, 0xe3, 0xd7, 0x8b, 0xa4, 0xcb, 0xa7, 0x0c, 0x01, 0xcd, 0x0e, 0x56, 0x3c, 0xa0, 0x97};
#else
        return Hash{0x00, 0x00, 0x00, 0x01, 0x48, 0xaa, 0xfe, 0x12, 0x82, 0x63, 0x23, 0xf7, 0x4d, 0x75, 0x30, 0x85, 0x2b, 0xe3, 0x2f, 0x64, 0x51, 0xd7, 0x42, 0x82, 0xa5, 0x50, 0x54, 0x01, 0x00, 0x74, 0x84, 0x22, 0xf5, 0xee, 0xae, 0xf2, 0xa8, 0xd5, 0xf0, 0x63, 0xf7, 0x8d, 0x35, 0x78, 0x3e, 0x71, 0xaf, 0x20, 0x6f, 0x73, 0x4d, 0x4e, 0x70, 0x01, 0x33, 0x16, 0x92, 0xe1, 0xad, 0x82, 0xd8, 0x28, 0x04, 0x9c};
#endif
    if (next % INTERVAL != 0)
        return p.target;
    auto cursor = parent;
    for (std::uint64_t i = 0; i < INTERVAL - 1; i++)
        cursor = entries_.at(cursor).header.previous;
    return retarget(p.target, entries_.at(cursor).header.timestamp, p.timestamp, network_);
}
Hash Chain::add(const Block &b) {
    auto block_size = encode(b).size();
    const auto &h = b.header;
    require(h.version == 1 && h.network == network_, "header version or network");
    auto parent = entries_.find(h.previous);
    require(parent != entries_.end(), "unknown parent");
    const auto &p = parent->second;
    require(p.header.height < std::numeric_limits<std::uint64_t>::max() &&
                h.height == p.header.height + 1,
            "height");
    require(h.timestamp <= 253402300799ULL, "timestamp range");
    std::vector<std::uint64_t> times;
    auto cursor = h.previous;
    for (unsigned i = 0; i < 11; i++) {
        const auto &a = entries_.at(cursor).header;
        times.push_back(a.timestamp);
        if (a.height == 0)
            break;
        cursor = a.previous;
    }
    std::sort(times.begin(), times.end());
    require(h.timestamp > times[times.size() / 2], "median time past");
    require(h.target == expected_target(h.previous), "unexpected target");
    require(valid_pow(h), "invalid proof of work");
    require(h.root == merkle(b.transactions), "merkle root");
    auto id = blockid(h);
    require(!entries_.contains(id), "known block");
    auto parent_view = view_at(h.previous);
    auto result = validate_transactions(b, parent_view);
    auto cumulative = sum_work(p.cumulative_work, work(h.target));
    bool best = greater_work(cumulative, entries_.at(tip_).cumulative_work);
    if (arrival_.size() >= budget_.blocks || block_bytes_ > budget_.block_bytes ||
        block_size > budget_.block_bytes - block_bytes_ || delta_count_ > budget_.delta_records ||
        result.delta.size() > budget_.delta_records - delta_count_)
        throw ResourceLimit(
            "local block index/delta budget exhausted; block not classified invalid");
    UtxoDelta active_delta;
    Utxos prepared;
    if (best) {
        parent_view.apply(result.delta, true);
        active_delta = parent_view.delta();
        auto coins = utxos_.size();
        for (const auto &change : active_delta) {
            if (change.before)
                coins--;
            if (change.after)
                coins++;
        }
        if (coins > budget_.active_coins)
            throw ResourceLimit("local active UTXO budget exhausted; block not classified invalid");
        prepared = prepare_commit(active_delta);
    }
    if (arrival_.size() == arrival_.capacity())
        arrival_.reserve(std::max<std::size_t>(16, arrival_.capacity() * 2));
    // All allocating work for the active state is complete before indexing/commit.
    auto added_records = result.delta.size();
    entries_.emplace(
        id, Entry{h, b.transactions, 0, 0, {}, std::move(result.delta), std::move(cumulative)});
    delta_count_ += added_records;
    block_bytes_ += block_size;
    arrival_.push_back(id);
    if (best) {
        commit_prepared(utxos_, active_delta, prepared);
        tip_ = id;
    }
    return id;
}
Block candidate(const Chain &c, Address recipient, std::uint64_t timestamp,
                const std::vector<Transaction> &transactions) {
    require(c.tip_header().height < std::numeric_limits<std::uint64_t>::max(),
            "candidate height overflow");
    Block b;
    auto &h = b.header;
    h.network = c.tip_header().network;
    h.height = c.tip_header().height + 1;
    h.previous = blockid(c.tip_header());
    h.timestamp = timestamp;
    h.target = c.expected_target(h.previous);
    Transaction cb;
    cb.coinbase_height = h.height;
    b.transactions.push_back(cb);
    b.transactions.insert(b.transactions.end(), transactions.begin(), transactions.end());
    // A sparse overlay validates fees without scanning or copying the full UTXO set.
    CoinsView view(c.utxos());
    auto result = validate_transactions(b, view);
    auto reward = add_money(subsidy(h.height), result.fees);
    if (reward > 0)
        b.transactions[0].outputs.push_back({reward, recipient});
    h.root = merkle(b.transactions);
    (void)encode(b);
    return b;
}
std::uint64_t mine(Block &b, std::uint64_t max_attempts) {
    for (std::uint64_t i = 0; i < max_attempts; i++) {
        if (valid_pow(b.header))
            return i + 1;
        if (b.header.nonce == std::numeric_limits<std::uint64_t>::max())
            throw Invalid("nonce exhausted");
        b.header.nonce++;
    }
    throw Invalid("mining attempt budget exhausted");
}
void Chain::save(const std::string &path) const {
    require(body_path_.empty() || !std::filesystem::exists(file_path(path)) ||
                !std::filesystem::equivalent(file_path(path), file_path(body_path_)),
            "snapshot cannot replace active body store");
    // Snapshot contains all branches, sorted by height then block id; replay validates everything.
    std::vector<const Entry *> sorted;
    for (const auto &[id, e] : entries_)
        if (e.header.height)
            sorted.push_back(&e);
    std::sort(sorted.begin(), sorted.end(), [](auto a, auto b) {
        if (a->header.height != b->header.height)
            return a->header.height < b->header.height;
        return blockid(a->header) < blockid(b->header);
    });
    Writer w;
    w.limit = 256 * 1024 * 1024;
    w.raw(Bytes{'Q', 'B', 'D', 'B', 1});
    w.u32(static_cast<std::uint32_t>(network_));
    w.raw(tip_);
    require(sorted.size() <= 250000, "snapshot block limit");
    w.u32(static_cast<std::uint32_t>(sorted.size()));
    for (auto e : sorted) {
        auto bytes = encode(block(blockid(e->header)));
        require(w.data.size() + 4 + bytes.size() + 64 <= 256 * 1024 * 1024, "snapshot file limit");
        w.blob(bytes);
    }
    w.raw(hash(w.data));
    storage::replace_atomic(path, w.data);
}
Chain Chain::load(Network n, const std::string &path) {
    {
        std::ifstream probe(file_path(path), std::ios::binary);
        std::array<char, 5> magic{};
        probe.read(magic.data(), 5);
        if (probe.gcount() == 5 && magic == std::array<char, 5>{'Q', 'B', 'J', 'L', 1})
            return load_journal(n, path);
    }
    std::ifstream f(file_path(path), std::ios::binary | std::ios::ate);
    require(bool(f), "snapshot open");
    auto size = f.tellg();
    require(size >= 0 && size <= 256 * 1024 * 1024, "snapshot file limit");
    Bytes data(static_cast<std::size_t>(size));
    f.seekg(0);
    f.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
    require(bool(f), "snapshot read");
    require(data.size() >= 64, "snapshot truncated");
    auto content = std::span<const std::uint8_t>(data).first(data.size() - 64);
    auto checksum = hash(content);
    require(std::equal(checksum.begin(), checksum.end(), data.end() - 64), "snapshot checksum");
    Reader r{content};
    require(r.fixed<5>() == std::array<std::uint8_t, 5>{'Q', 'B', 'D', 'B', 1}, "snapshot format");
    require(r.u32() == static_cast<std::uint32_t>(n), "snapshot network");
    auto saved_tip = r.fixed<64>();
    auto count = r.u32();
    require(count <= 250000, "snapshot count");
    Chain c(n);
    for (std::uint32_t i = 0; i < count; i++)
        c.add(decode_block(r.blob()));
    r.end();
    c.restore_tip(saved_tip);
    c.snapshot_origin_ = path;
    return c;
}
} // namespace qbtc
