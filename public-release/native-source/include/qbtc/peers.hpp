#pragma once
#include "qbtc/net.hpp"
#include <map>
namespace qbtc::net {
struct PeerStorageFailure : std::runtime_error { using std::runtime_error::runtime_error; };
enum class PeerScope : std::uint32_t { Loopback = 0, Lan = 1, Public = 2 };
inline constexpr std::size_t MAX_KNOWN_PEERS = 64;
inline constexpr std::size_t MAX_SHARED_PEERS = 16;
inline constexpr std::size_t MAX_OUTBOUND_PEERS = 8;
bool peer_in_scope(const Endpoint &peer, PeerScope scope);
Bytes encode_peers(std::span<const Endpoint> peers);
std::vector<Endpoint> decode_peers(std::span<const std::uint8_t> bytes);
// Only endpoints that completed a validated synchronization round are saved.
// Scheduling uses injected monotonic milliseconds, never remote timestamps.
class PeerBook {
    struct Record {
        bool verified = false;
        bool configured = false;
        unsigned failures = 0;
        std::int64_t retry = 0;
        std::uint64_t last_attempt = 0;
    };
    std::string path_;
    PeerScope scope_;
    std::vector<Endpoint> blocked_;
    std::map<Endpoint, Record> records_;
    std::uint64_t sequence_ = 0;
    Endpoint canonical(Endpoint peer) const;
    bool permitted(const Endpoint &peer) const;
    void save() const;
  public:
    PeerBook(std::string path, PeerScope scope, std::vector<Endpoint> blocked);
    bool add(Endpoint peer, bool configured = false);
    std::vector<Endpoint> known() const;
    std::vector<Endpoint> verified() const;
    std::vector<Endpoint> due(std::int64_t now, std::size_t slots);
    void good(Endpoint peer, std::int64_t now);
    void bad(Endpoint peer, std::int64_t now);
};
} // namespace qbtc::net
