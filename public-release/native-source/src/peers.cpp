#include "qbtc/platform.hpp"
#include "qbtc/peers.hpp"
#include "qbtc/storage.hpp"
#include <algorithm>
#include <filesystem>
#include <limits>
#include <set>
namespace qbtc::net {
namespace {
void need(bool ok, const char *message) { if (!ok) throw Invalid(message); }
void put32(Bytes &bytes, std::uint32_t value) {
    for (unsigned j = 0; j < 4; ++j)
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * j)));
}
std::uint32_t get32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    need(offset <= bytes.size() && bytes.size() - offset >= 4, "truncated peer data");
    std::uint32_t value = 0;
    for (unsigned j = 0; j < 4; ++j) value |= std::uint32_t(bytes[offset + j]) << (8 * j);
    return value;
}
std::uint32_t ip_value(const Endpoint &peer) {
    in_addr parsed{};
    need(inet_pton(AF_INET, peer.host.c_str(), &parsed) == 1, "peer numeric IPv4 required");
    return ntohl(parsed.s_addr);
}
std::string ip_text(std::uint32_t value) {
    return std::to_string(value >> 24) + "." + std::to_string((value >> 16) & 255) + "." +
           std::to_string((value >> 8) & 255) + "." + std::to_string(value & 255);
}
} // namespace
bool peer_in_scope(const Endpoint &peer, PeerScope scope) {
    try { ipv4_address(peer.host, true); } catch (const Invalid &) { return false; }
    if (peer.number < 1024) return false;
    auto ip = ip_value(peer);
    bool loopback = (ip >> 24) == 127;
    bool lan = (ip >> 24) == 10 || (ip >> 20) == 0xac1 || (ip >> 16) == 0xc0a8;
    if (scope == PeerScope::Loopback) return loopback;
    if (scope == PeerScope::Lan) return lan;
    return scope == PeerScope::Public && !loopback && !lan;
}
Bytes encode_peers(std::span<const Endpoint> peers) {
    need(peers.size() <= MAX_SHARED_PEERS, "shared peer count limit");
    Bytes bytes;
    put32(bytes, static_cast<std::uint32_t>(peers.size()));
    for (const auto &peer : peers) {
        ipv4_address(peer.host, true);
        need(peer.number >= 1024, "peer port range");
        put32(bytes, ip_value(peer)); put32(bytes, peer.number);
    }
    return bytes;
}
std::vector<Endpoint> decode_peers(std::span<const std::uint8_t> bytes) {
    auto count = get32(bytes, 0);
    need(count <= MAX_SHARED_PEERS && bytes.size() == 4 + std::size_t(count) * 8,
         "peer list count or size");
    std::vector<Endpoint> peers;
    std::set<std::pair<std::string, std::uint16_t>> seen;
    for (std::uint32_t j = 0; j < count; ++j) {
        auto offset = 4 + std::size_t(j) * 8;
        auto port_number = get32(bytes, offset + 4);
        need(port_number >= 1024 && port_number <= 65535, "peer port range");
        Endpoint peer{ip_text(get32(bytes, offset)), static_cast<std::uint16_t>(port_number), true};
        ipv4_address(peer.host, true);
        need(seen.emplace(peer.host, peer.number).second, "duplicate shared peer");
        peers.push_back(peer);
    }
    return peers;
}
Endpoint PeerBook::canonical(Endpoint peer) const {
    peer.internet_test = scope_ == PeerScope::Public;
    return peer;
}
bool PeerBook::permitted(const Endpoint &peer) const {
    if (!peer_in_scope(peer, scope_)) return false;
    for (const auto &blocked : blocked_)
        if (blocked.host == peer.host && blocked.number == peer.number) return false;
    return true;
}
PeerBook::PeerBook(std::string path, PeerScope scope, std::vector<Endpoint> blocked)
    : path_(std::move(path)), scope_(scope), blocked_(std::move(blocked)) {
    need(scope <= PeerScope::Public, "peer store scope");
    if (!std::filesystem::exists(file_path(path_))) return;
    auto bytes = storage::read_bounded(path_, 1024);
    need(bytes.size() >= 145, "peer store size");
    auto body = std::span<const std::uint8_t>(bytes).first(bytes.size() - 64);
    auto digest = hash(body);
    need(std::equal(digest.begin(), digest.end(), bytes.end() - 64), "peer store checksum");
    constexpr std::array<std::uint8_t, 5> magic{'Q','B','P','R',1};
    need(std::equal(magic.begin(), magic.end(), body.begin()), "peer store format");
    need(get32(body, 5) == static_cast<std::uint32_t>(ACTIVE_NETWORK), "peer store network");
    need(get32(body, 9) == static_cast<std::uint32_t>(scope_), "peer store scope mismatch");
    auto genesis_id = blockid(genesis(ACTIVE_NETWORK).header);
    need(std::equal(genesis_id.begin(), genesis_id.end(), body.begin() + 13), "peer store genesis");
    auto count = get32(body, 77);
    need(count <= MAX_KNOWN_PEERS && body.size() == 81 + std::size_t(count) * 8,
         "peer store count or size");
    for (std::uint32_t j = 0; j < count; ++j) {
        auto offset = 81 + std::size_t(j) * 8;
        auto port_number = get32(body, offset + 4);
        need(port_number >= 1024 && port_number <= 65535, "peer store port");
        auto peer = canonical({ip_text(get32(body, offset)), static_cast<std::uint16_t>(port_number)});
        need(peer_in_scope(peer, scope_), "peer store address scope");
        // Old self addresses may legitimately become local after reconfiguration.
        if (!permitted(peer)) continue;
        need(records_.emplace(peer, Record{true}).second, "duplicate peer store entry");
    }
}
bool PeerBook::add(Endpoint peer, bool configured) {
    peer = canonical(peer);
    if (!permitted(peer) || records_.contains(peer) || records_.size() >= MAX_KNOWN_PEERS) return false;
    if (!configured && scope_ == PeerScope::Public) {
        unsigned group_count = 0;
        auto group = ip_value(peer) >> 16;
        for (const auto &[entry, record] : records_) {
            (void)record;
            if ((ip_value(entry) >> 16) == group) ++group_count;
        }
        if (group_count >= 4) return false;
    }
    records_.emplace(std::move(peer), Record{false, configured});
    return true;
}
std::vector<Endpoint> PeerBook::known() const {
    std::vector<Endpoint> result;
    for (const auto &[peer, record] : records_) { (void)record; result.push_back(peer); }
    return result;
}
std::vector<Endpoint> PeerBook::verified() const {
    std::vector<Endpoint> result;
    for (const auto &[peer, record] : records_) if (record.verified) result.push_back(peer);
    return result;
}
std::vector<Endpoint> PeerBook::due(std::int64_t now, std::size_t slots) {
    need(now >= 0, "peer scheduler time");
    slots = std::min(slots, MAX_OUTBOUND_PEERS);
    std::vector<Endpoint> candidates;
    for (const auto &[peer, record] : records_) if (record.retry <= now) candidates.push_back(peer);
    std::stable_sort(candidates.begin(), candidates.end(), [&](const auto &a, const auto &b) {
        return records_.at(a).last_attempt < records_.at(b).last_attempt;
    });
    if (candidates.size() > slots) candidates.resize(slots);
    for (const auto &peer : candidates) {
        auto &record = records_.at(peer);
        record.last_attempt = ++sequence_;
        record.retry = std::numeric_limits<std::int64_t>::max(); // in flight
    }
    return candidates;
}
void PeerBook::good(Endpoint peer, std::int64_t now) {
    auto &record = records_.at(canonical(peer));
    bool first = !record.verified;
    record.verified = true; record.failures = 0; record.retry = now + 500;
    if (first) save();
}
void PeerBook::bad(Endpoint peer, std::int64_t now) {
    auto &record = records_.at(canonical(peer));
    record.failures = std::min(record.failures + 1, 9u);
    record.retry = now + std::min<std::int64_t>(300000, std::int64_t(1000) << record.failures);
    // Untrusted unreachable candidates cannot occupy the finite book forever.
    // Explicit seeds and previously verified peers survive temporary outages.
    if (!record.verified && !record.configured && record.failures >= 3)
        records_.erase(canonical(peer));
}
void PeerBook::save() const {
    Bytes body{'Q','B','P','R',1};
    put32(body, static_cast<std::uint32_t>(ACTIVE_NETWORK));
    put32(body, static_cast<std::uint32_t>(scope_));
    auto genesis_id = blockid(genesis(ACTIVE_NETWORK).header);
    body.insert(body.end(), genesis_id.begin(), genesis_id.end());
    auto entries = verified();
    put32(body, static_cast<std::uint32_t>(entries.size()));
    for (const auto &peer : entries) { put32(body, ip_value(peer)); put32(body, peer.number); }
    auto digest = hash(body); body.insert(body.end(), digest.begin(), digest.end());
    try { storage::replace_atomic(path_, body); }
    catch (const std::exception &e) { throw PeerStorageFailure(std::string("peer store write failed: ") + e.what()); }
}
} // namespace qbtc::net
