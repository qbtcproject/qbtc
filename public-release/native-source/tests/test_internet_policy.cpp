#include "qbtc/net.hpp"
#include <iostream>
using namespace qbtc;
void need(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
int main() {
#ifdef _WIN32
    WSADATA data{};
    need(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Winsock startup");
#endif
    unsigned checks = 0;
    for (auto ip : {"127.0.0.1", "10.1.2.3", "172.16.0.1", "172.31.255.254", "192.168.1.1"}) {
        net::ipv4_address(ip, false); ++checks;
        net::ipv4_address(ip, true); ++checks;
    }
    for (auto ip : {"1.1.1.1", "8.8.8.8", "172.32.0.1", "126.0.0.1", "198.17.255.254", "198.20.0.1"}) {
        bool rejected = false;
        try { net::ipv4_address(ip, false); } catch (const Invalid &) { rejected = true; }
        need(rejected, "default must reject public peer"); ++checks;
        net::ipv4_address(ip, true); ++checks;
    }
    for (auto ip : {"0.0.0.0", "0.1.2.3", "169.254.1.1", "100.64.0.1", "100.127.255.254",
                    "192.0.0.1", "192.0.2.1", "192.88.99.1", "198.18.0.1", "198.19.255.254",
                    "198.51.100.1", "203.0.113.1", "224.0.0.1", "255.255.255.255", "localhost",
                    "::1", "1.2.3.999", "1.2.3.4:5"}) {
        for (bool enabled : {false, true}) {
            bool rejected = false;
            try { net::ipv4_address(ip, enabled); } catch (const Invalid &) { rejected = true; }
            need(rejected, "special address must be rejected"); ++checks;
        }
    }
    need(net::endpoint("8.8.8.8:19001", true).internet_test, "endpoint retains opt-in"); ++checks;
    std::vector<net::Endpoint> configured{{"8.8.8.8", 19001, true}};
    for (auto ip : {"8.8.8.8", "1.1.1.1", "192.168.1.2", "127.0.0.1"}) {
        in_addr address{};
        need(inet_pton(AF_INET, ip, &address) == 1, "test source parse");
        bool expected = std::string_view(ip) == "8.8.8.8" || std::string_view(ip) == "127.0.0.1";
        need(net::incoming_source_allowed(ntohl(address.s_addr), true, configured) == expected,
             "Internet source allowlist"); ++checks;
        bool local = std::string_view(ip) == "192.168.1.2" || std::string_view(ip) == "127.0.0.1";
        need(net::incoming_source_allowed(ntohl(address.s_addr), false, configured) == local,
             "default private source policy"); ++checks;
    }
    std::cout << "PASS internet address policy " << checks << " checks\n";
}
