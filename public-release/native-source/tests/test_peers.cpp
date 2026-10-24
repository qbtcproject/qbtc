#include "qbtc/peers.hpp"
#include "qbtc/storage.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
using namespace qbtc;
using namespace qbtc::net;
namespace {
unsigned checks = 0;
void check(bool ok, const char *message) {
    ++checks; if (!ok) throw std::runtime_error(message);
}
template<class F> void rejects(F f) {
    bool rejected = false;
    try { f(); } catch (const Invalid &) { rejected = true; }
    check(rejected, "expected Invalid rejection");
}
void resign(Bytes &b) {
    b.resize(b.size() - 64); auto digest = hash(b); b.insert(b.end(), digest.begin(), digest.end());
}
}
int main() {
#ifdef _WIN32
    WSADATA data{}; check(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Winsock");
#endif
    auto dir = std::filesystem::temp_directory_path() / ("qbtc-peers-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(dir);
    try {
        Endpoint a{"127.0.0.1", 19001}, b{"127.0.0.1", 19002}, c{"127.0.0.1", 19003};
        auto bytes = encode_peers(std::array<Endpoint, 2>{a, b});
        auto decoded = decode_peers(bytes);
        check(decoded.size() == 2 && decoded[1].number == 19002, "full endpoint wire roundtrip");
        for (std::size_t size = 0; size < bytes.size(); ++size)
            rejects([&] { decode_peers(std::span<const std::uint8_t>(bytes).first(size)); });
        auto extra = bytes; extra.push_back(0); rejects([&] { decode_peers(extra); });
        auto duplicate = encode_peers(std::array<Endpoint, 2>{a, a});
        rejects([&] { decode_peers(duplicate); });
        auto oversized = bytes; oversized[0] = 17; rejects([&] { decode_peers(oversized); });
        auto bad_port = bytes; bad_port[8] = 0; bad_port[9] = 0; bad_port[10] = 0; bad_port[11] = 0;
        rejects([&] { decode_peers(bad_port); });
        check(!peer_in_scope(a, PeerScope::Public), "no loopback learned from public peer");
        check(!peer_in_scope({"192.168.1.2",19001}, PeerScope::Public), "no LAN learned from public peer");
        check(!peer_in_scope({"8.8.8.8",19001}, PeerScope::Lan), "LAN scope stays LAN");
        check(!peer_in_scope({"169.254.169.254",19001}, PeerScope::Public), "metadata endpoint rejected");
        check(!peer_in_scope({"100.64.1.1",19001}, PeerScope::Public), "CGNAT endpoint rejected");
        check(peer_in_scope({"8.8.8.8",19001}, PeerScope::Public), "public unicast accepted");
        auto path = (dir / "verified.peers").string();
        PeerBook book(path, PeerScope::Loopback, {a});
        check(!book.add(a), "self not learned");
        check(book.add(b) && book.add(c), "candidates learned");
        check(!std::filesystem::exists(path), "unverified announcements not persisted");
        auto due = book.due(0,8); check(due.size() == 2, "initial attempts");
        check(book.due(0,8).empty(), "inflight entries never scheduled twice");
        book.good(b,100); book.bad(c,100);
        check(book.due(599,8).empty(), "success delay respected");
        due = book.due(600,8); check(due.size() == 1 && due[0].number == b.number, "failed peer backs off");
        book.good(b,600);
        PeerBook restored(path, PeerScope::Loopback, {a});
        check(restored.known().size() == 1 && restored.verified().size() == 1,
              "only independently checked peer survives restart");
        PeerBook without_seeds(path, PeerScope::Loopback, {a});
        check(without_seeds.due(0,8).size() == 1, "restart reconnects without original seeds");
        auto original = storage::read_bounded(path,1024);
        for (std::size_t i = 0; i < original.size(); ++i) {
            auto corrupted = original; corrupted[i] ^= 1; storage::replace_atomic(path,corrupted);
            rejects([&] { PeerBook broken(path, PeerScope::Loopback, {a}); });
            check(storage::read_bounded(path,1024) == corrupted, "corruption preserved for diagnosis");
        }
        storage::replace_atomic(path,original);
        rejects([&] { PeerBook wrong_scope(path, PeerScope::Public, {}); });
        auto other_network = original; other_network[5] ^= 1; resign(other_network);
        storage::replace_atomic(path,other_network);
        rejects([&] { PeerBook wrong_network(path, PeerScope::Loopback, {}); });
        auto other_genesis = original; other_genesis[13] ^= 1; resign(other_genesis);
        storage::replace_atomic(path,other_genesis);
        rejects([&] { PeerBook wrong_genesis(path, PeerScope::Loopback, {}); });
        PeerBook full((dir / "full").string(),PeerScope::Loopback,{});
        for (std::uint16_t p = 10000; p < 10064; ++p)
            check(full.add({"127.0.0.1",p}), "bounded candidates accepted");
        check(!full.add({"127.0.0.1",11000}), "candidate capacity enforced");
        auto attempts = full.due(0,100); check(attempts.size() == 8, "outbound burst capped at eight");
        check(full.due(0,0).empty(), "no requests when all slots occupied");
        for (const auto &p : attempts) full.bad(p,0);
        auto other_attempts = full.due(1,8);
        check(other_attempts.size() == 8, "dead seeds do not block unexplored candidates");
        for (const auto &p : other_attempts)
            check(std::find(attempts.begin(),attempts.end(),p) == attempts.end(), "fair exploration");
        PeerBook diversity((dir / "public").string(),PeerScope::Public,{});
        for (unsigned i = 1; i <= 4; ++i)
            check(diversity.add({"8.8.0." + std::to_string(i),19001}), "public group admission");
        check(!diversity.add({"8.8.0.5",19001}), "learned /16 group limit");
        check(diversity.add({"8.8.0.5",19001},true), "explicit user seeds may share a group");
        PeerBook doomed((dir / "doomed").string(),PeerScope::Loopback,{});
        doomed.add(b);
        for (auto time : {0,2000,6000}) {
            check(doomed.due(time,8).size() == 1, "retry becomes eligible after backoff");
            doomed.bad(b,time);
        }
        check(doomed.known().empty(), "unverified unreachable candidates are evicted");
        PeerBook explicit_seed((dir / "seed").string(),PeerScope::Loopback,{});
        explicit_seed.add(b,true);
        for (auto time : {0,2000,6000}) { explicit_seed.due(time,8); explicit_seed.bad(b,time); }
        check(explicit_seed.known().size() == 1, "explicit seed survives an outage");
        auto fail_path = (dir / "write-failure").string();
        PeerBook fail(fail_path,PeerScope::Loopback,{}); fail.add(b);
        std::filesystem::create_directory(fail_path);
        bool fatal = false;
        try { fail.good(b,0); } catch (const PeerStorageFailure &) { fatal = true; }
        check(fatal && std::filesystem::is_directory(fail_path), "failed durable store stops safely");
        std::filesystem::remove_all(dir);
        std::cout << "PASS peer discovery/persistence " << checks
                  << " checks (scope, corrupt store, restart, capacity, fairness, backoff)\n";
    } catch (...) { std::filesystem::remove_all(dir); throw; }
}
