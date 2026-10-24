#include "qbtc/net.hpp"
#include "qbtc/peers.hpp"
#include "qbtc/mempool.hpp"
#include <charconv>
#include <chrono>
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#endif
namespace qbtc::net {
namespace {
bool onion_adapter_enabled = false;
void need(bool ok, const char *text) {
    if (!ok)
        throw Invalid(text);
}
void put32(Bytes &b, std::uint32_t n) {
    for (unsigned j = 0; j < 4; j++)
        b.push_back(static_cast<std::uint8_t>(n >> (8 * j)));
}
struct Reader {
    std::span<const std::uint8_t> data;
    std::size_t pos = 0;
    std::span<const std::uint8_t> take(std::size_t n) {
        need(n <= data.size() - pos, "truncated network payload");
        auto r = data.subspan(pos, n);
        pos += n;
        return r;
    }
    std::uint32_t u32() {
        auto b = take(4);
        std::uint32_t n = 0;
        for (unsigned j = 0; j < 4; j++)
            n |= std::uint32_t(b[j]) << (8 * j);
        return n;
    }
};
} // namespace
void close_socket(Socket s) {
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
}
Connection::Connection(Socket value) : s(value) {}
Connection::~Connection() {
    if (s != BAD)
        close_socket(s);
}
void enable_onion_adapter() { onion_adapter_enabled = true; }
void timeout(Socket s) {
#ifdef _WIN32
    DWORD ms = 1500;
    need(setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&ms), sizeof(ms)) ==
             0,
         "receive timeout");
    need(setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&ms), sizeof(ms)) ==
             0,
         "send timeout");
#else
    timeval tv{1, 500000};
    need(setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0, "receive timeout");
    need(setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0, "send timeout");
#endif
}
void transfer_until(Socket s, std::span<std::uint8_t> bytes, bool sending,
                    std::chrono::steady_clock::time_point deadline, bool bounded_response = false) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        need(std::chrono::steady_clock::now() < deadline, "network transfer deadline");
        if (!sending && bounded_response) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            need(remaining > 0, "network transfer deadline");
#ifdef _WIN32
            const DWORD milliseconds = static_cast<DWORD>(remaining);
            need(setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                           reinterpret_cast<const char *>(&milliseconds), sizeof(milliseconds)) == 0,
                 "bounded response timeout");
#else
            timeval wait{static_cast<long>(remaining / 1000), static_cast<long>((remaining % 1000) * 1000)};
            need(setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait)) == 0,
                 "bounded response timeout");
#endif
        }
        int n = 0;
#ifdef _WIN32
        auto count = static_cast<int>(bytes.size() - offset);
        n = sending ? ::send(s, reinterpret_cast<const char *>(bytes.data() + offset), count, 0)
                    : ::recv(s, reinterpret_cast<char *>(bytes.data() + offset), count, 0);
#else
        auto count = bytes.size() - offset;
        auto r = sending ? ::send(s, bytes.data() + offset, count, MSG_NOSIGNAL)
                         : ::recv(s, bytes.data() + offset, count, 0);
        need(r > 0, "network closed or timed out");
        n = static_cast<int>(r);
#endif
        need(n > 0, "network closed or timed out");
        offset += static_cast<std::size_t>(n);
    }
}
void transfer(Socket s, std::span<std::uint8_t> bytes, bool sending) {
    transfer_until(s, bytes, sending, std::chrono::steady_clock::now() + std::chrono::seconds(3));
}
void send_frame(Socket s, char command, const Bytes &payload) {
    need(payload.size() <= PAYLOAD_LIMIT, "network payload too large");
    Bytes frame;
    put32(frame, static_cast<std::uint32_t>(ACTIVE_NETWORK));
    put32(frame, 1);
    frame.push_back(static_cast<std::uint8_t>(command));
    put32(frame, static_cast<std::uint32_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    transfer(s, frame, true);
}
static Frame receive_frame_with_wait(Socket socket, std::string_view allowed,
                                     std::chrono::seconds response_wait, char response_to = 0) {
    std::array<std::uint8_t, 13> header{};
    auto deadline = std::chrono::steady_clock::now() + response_wait;
    transfer_until(socket, header, false, deadline, response_wait > std::chrono::seconds(3));
    Reader reader{header};
    need(reader.u32() == static_cast<std::uint32_t>(ACTIVE_NETWORK), "wrong network");
    need(reader.u32() == 1, "wrong wire version");
    Frame frame;
    frame.command = static_cast<char>(reader.take(1)[0]);
    need(allowed.empty() || allowed.find(frame.command) != std::string_view::npos,
         "command not permitted on this endpoint");
    auto length = reader.u32();
    need(length <= PAYLOAD_LIMIT, "network length limit");
    if (response_to) {
        need(frame.command == response_to || frame.command == 'E', "response command mismatch");
        if (frame.command == 'E') need(length <= 256, "error response length");
        else if (response_to == 'D')
            need(length >= 4 && length <= 132 && (length - 4) % 8 == 0,
                 "discovery response length");
        else if (response_to == 'A')
            need(length >= 4 && length <= 36 && (length - 4) % 4 == 0,
                 "legacy address response length");
        else if (response_to == 'S' || response_to == 'M')
            need(length <= 2048, "status response length");
    }
    if (!allowed.empty()) {
        bool valid_length = false;
        switch (frame.command) {
        case 'S':
            valid_length = allowed == "MS" ? length == 72 : length == 0;
            break;
        case 'P':
            valid_length = length == 0;
            break;
        case 'D':
            valid_length = length == 0 || length == 12;
            break;
        case 'A':
            valid_length = length == 0 || length == 4;
            break;
        case 'B':
            valid_length = length >= 4 && length <= 4 + 65 * 64 && (length - 4) % 64 == 0;
            break;
        case 'T':
            valid_length = length <= mempool::MAX_RELAY_BYTES;
            break;
        case 'M':
            valid_length = length == 108;
            break;
        default:
            break;
        }
        need(valid_length, "request length invalid for command");
    }
    frame.payload.resize(length);
    transfer_until(socket, frame.payload, false, deadline, response_wait > std::chrono::seconds(3));
    return frame;
}
Frame receive_frame(Socket socket, std::string_view allowed) {
    return receive_frame_with_wait(socket, allowed, std::chrono::seconds(3));
}
std::uint16_t port(std::string_view text) {
    unsigned value = 0;
    auto [p, e] = std::from_chars(text.data(), text.data() + text.size(), value);
    need(e == std::errc{} && p == text.data() + text.size() && value >= 1024 && value <= 65535,
         "port must be 1024..65535");
    return static_cast<std::uint16_t>(value);
}
std::string ipv4_address(std::string_view text, bool internet_test) {
    std::string address(text);
    in_addr parsed{};
    need(inet_pton(AF_INET, address.c_str(), &parsed) == 1, "numeric IPv4 address required");
    auto value = ntohl(parsed.s_addr);
    bool allowed = (value >> 24) == 127 || (value >> 24) == 10 || (value >> 20) == 0xac1 ||
                   (value >> 16) == 0xc0a8;
    // Opt-in unicast only: never multicast, unspecified, broadcast, link-local,
    // CGNAT, documentation, benchmarking or other reserved address ranges.
    const auto first = value >> 24;
    const bool special = first == 0 || first >= 224 ||
        (value >> 22) == 0x191 || (value >> 16) == 0xa9fe ||
        ((value >> 24) == 192 && ((value >> 8) == 0xc00000 ||
        (value >> 8) == 0xc00002 || (value >> 8) == 0xc05863)) ||
        (value >> 17) == 0x6309 || (value >> 8) == 0xc63364 ||
        (value >> 8) == 0xcb0071;
    need(allowed || (internet_test && !special),
         "only loopback or RFC1918 private IPv4 allowed unless --internet-test is enabled");
    return address;
}
bool incoming_source_allowed(std::uint32_t source_ip, bool internet_test,
                             std::span<const Endpoint> configured) {
    if ((source_ip >> 24) == 127)
        return true;
    if (!internet_test)
        return (source_ip >> 24) == 10 || (source_ip >> 20) == 0xac1 ||
               (source_ip >> 16) == 0xc0a8;
    for (const auto &remote : configured) {
        in_addr address{};
        if (inet_pton(AF_INET, remote.host.c_str(), &address) == 1 &&
            ntohl(address.s_addr) == source_ip)
            return true;
    }
    return false;
}
std::string private_address(std::string_view text) {
    return ipv4_address(text, false);
}
Endpoint endpoint(std::string_view text, bool internet_test) {
    auto colon = text.find(':');
    if (colon == std::string_view::npos)
        return {"127.0.0.1", port(text), internet_test};
    return {ipv4_address(text.substr(0, colon), internet_test), port(text.substr(colon + 1)), internet_test};
}
Bytes request(std::uint16_t target, char command, const Bytes &payload) {
    return request(Endpoint{"127.0.0.1", target}, command, payload);
}
void connect_bounded(Socket socket, const sockaddr_in &address) {
#ifdef _WIN32
    u_long mode = 1;
    need(ioctlsocket(socket, static_cast<long>(FIONBIO), &mode) == 0, "connect nonblocking mode");
#else
    need(socket < FD_SETSIZE, "connect descriptor limit");
    auto flags = fcntl(socket, F_GETFL, 0);
    need(flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0, "connect nonblocking mode");
#endif
    auto result = ::connect(socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address));
    if (result != 0) {
#ifdef _WIN32
        auto error = WSAGetLastError();
        need(error == WSAEWOULDBLOCK || error == WSAEINPROGRESS, "peer connection failed");
#else
        need(errno == EINPROGRESS, "peer connection failed");
#endif
        fd_set writes;
        FD_ZERO(&writes);
        FD_SET(socket, &writes);
        fd_set errors;
        FD_ZERO(&errors);
        FD_SET(socket, &errors);
        timeval deadline{3, 0};
        need(::select(static_cast<int>(socket + 1), nullptr, &writes, &errors, &deadline) > 0,
             "peer connect deadline");
        int error_code = 0;
#ifdef _WIN32
        int size = sizeof(error_code);
#else
        socklen_t size = sizeof(error_code);
#endif
        need(getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error_code),
                        &size) == 0 &&
                 error_code == 0,
             "peer connection failed");
    }
#ifdef _WIN32
    mode = 0;
    need(ioctlsocket(socket, static_cast<long>(FIONBIO), &mode) == 0, "restore socket mode");
#else
    need(fcntl(socket, F_SETFL, flags) == 0, "restore socket mode");
#endif
}
Bytes request(const Endpoint &target, char command, const Bytes &payload) {
    Connection c(::socket(AF_INET, SOCK_STREAM, 0));
    need(c.s != BAD, "client socket");
    timeout(c.s);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(target.number);
    need(inet_pton(AF_INET, ipv4_address(target.host, target.internet_test).c_str(), &addr.sin_addr) == 1,
         "request IPv4 address");
    need(command != 'M' || target.host == "127.0.0.1", "mining control is loopback-only");
    connect_bounded(c.s, addr);
    send_frame(c.s, command, payload);
    // Only the local mining client waits longer for processing. Incoming
    // requests, P2P responses and send deadlines retain their existing limits.
    const bool local_mining = command == 'M' && target.host == "127.0.0.1";
    const bool onion_wait = onion_adapter_enabled && target.host == "127.0.0.1" && (target.number == 19022 || target.number == 19024 || target.number == 19026 || target.number == 19028) && command != 'M';
    const unsigned wait_seconds = local_mining ? 120u : (onion_wait ? 90u : 3u);
    if (local_mining || onion_wait) {
#ifdef _WIN32
        DWORD wait_ms = wait_seconds * 1000;
        need(setsockopt(c.s, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char *>(&wait_ms), sizeof(wait_ms)) == 0,
             "local mining response timeout");
#else
        timeval wait{static_cast<long>(wait_seconds), 0};
        need(setsockopt(c.s, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait)) == 0,
             "local mining response timeout");
#endif
    }
    Frame response;
    try {
        response = receive_frame_with_wait(c.s, {}, std::chrono::seconds(wait_seconds), command);
    } catch (const Invalid &) {
        if (local_mining)
            throw Invalid("Mining response unavailable; blocks may already be committed. "
                          "Check node status before issuing another mine command. "
                          "This client does not retry automatically.");
        throw;
    }
    if (response.command == 'E')
        throw Invalid(std::string(response.payload.begin(), response.payload.end()));
    need(response.command == command, "response command mismatch");
    Bytes result;
    result.swap(response.payload);
    return result;
}
SyncReply fetch_sync(const Endpoint &target, const Bytes &locator, std::uint16_t advertise_port,
                     bool discover, std::optional<Endpoint> advertise) {
    SyncReply reply;
    auto blocks = request(target, 'B', locator);
    reply.blocks.swap(blocks);
    auto transactions = request(target, 'P', {});
    reply.transactions.swap(transactions);
    Bytes announcement;
    if (advertise_port)
        put32(announcement, advertise_port);
    auto addresses = request(target, 'A', announcement);
    reply.advertised.swap(addresses);
    if (discover) {
        Bytes announcement;
        if (advertise) announcement = encode_peers(std::array<Endpoint, 1>{*advertise});
        try { reply.discovered = request(target, 'D', announcement); }
        catch (const Invalid &e) {
            // Only an explicit legacy/disabled-command reply permits fallback.
            // Truncated, oversized or otherwise malformed responses are peer failures.
            std::string_view reason = e.what();
            if (reason != "command not permitted on this endpoint" &&
                reason != "peer discovery disabled" && reason != "unknown network command")
                throw;
        }
    }
    return reply;
}

} // namespace qbtc::net
