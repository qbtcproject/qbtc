#pragma once
#include "qbtc/consensus.hpp"
#include <optional>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
constexpr Socket BAD = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
constexpr Socket BAD = -1;
#endif
namespace qbtc::net {
inline constexpr std::uint32_t PAYLOAD_LIMIT = MAX_BLOCK_BYTES + 4096;
void close_socket(Socket socket);
class Connection {
  public:
    Socket s = BAD;
    explicit Connection(Socket value);
    ~Connection();
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
};
struct Frame {
    char command = 0;
    Bytes payload;
};
// Explicit opt-in; only requests to the loopback onion adapter receive longer waits.
void enable_onion_adapter();
void timeout(Socket socket);
void transfer(Socket socket, std::span<std::uint8_t> bytes, bool sending);
void send_frame(Socket socket, char command, const Bytes &payload);
Frame receive_frame(Socket socket, std::string_view allowed = {});
std::uint16_t port(std::string_view text);
struct Endpoint {
    std::string host = "127.0.0.1";
    std::uint16_t number = 0;
    bool internet_test = false;
    auto operator<=>(const Endpoint &) const = default;
};
bool incoming_source_allowed(std::uint32_t source_ip, bool internet_test,
                             std::span<const Endpoint> configured);
std::string private_address(std::string_view text);
std::string ipv4_address(std::string_view text, bool internet_test);
Endpoint endpoint(std::string_view text, bool internet_test = false);
Bytes request(const Endpoint &target, char command, const Bytes &payload);
Bytes request(std::uint16_t port, char command, const Bytes &payload);
struct SyncReply {
    Bytes blocks, transactions, advertised, discovered;
};
SyncReply fetch_sync(const Endpoint &target, const Bytes &locator,
                     std::uint16_t advertise_port = 0, bool discover = false,
                     std::optional<Endpoint> advertise = std::nullopt);
} // namespace qbtc::net
