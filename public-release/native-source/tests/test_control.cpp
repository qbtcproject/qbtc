#include "qbtc/control.hpp"
#include <filesystem>
#include <functional>
#include <iostream>
#include <openssl/rand.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif
using namespace qbtc;
namespace {
int checks = 0;
void check(bool ok, const char *text) {
    if (!ok)
        throw std::runtime_error(text);
    checks++;
}
void rejects(const std::function<void()> &f, const char *expected) {
    try {
        f();
    } catch (const Invalid &e) {
        check(std::string(e.what()).find(expected) != std::string::npos, e.what());
        return;
    }
    throw std::runtime_error(expected);
}
} // namespace
int main() {
    std::filesystem::path dir;
    try {
        Bytes random(8);
        check(RAND_bytes(random.data(), 8) == 1, "temp randomness");
        dir = std::filesystem::temp_directory_path() / ("qbtc-control-" + hex(random));
        std::filesystem::create_directory(dir);
        auto path = (dir / "node.control").string();
        control::Authenticator server(path, true), client(path, false),
            other((dir / "other.control").string(), true);
        Bytes body(36, 0);
        body.back() = 1;
        std::uint64_t now = 1000000;
        auto sealed = client.seal('M', body, now);
        check(server.open('M', sealed, now) == body, "authenticated body");
        rejects([&] { server.open('M', sealed, now); }, "replay");
        auto bad = client.seal('M', body, now);
        bad[45] ^= 1;
        rejects([&] { server.open('M', bad, now); }, "authentication");
        auto wrong = other.seal('M', body, now);
        rejects([&] { server.open('M', wrong, now); }, "authentication");
        auto command = client.seal('M', body, now);
        rejects([&] { server.open('T', command, now); }, "authentication");
        auto expired = client.seal('M', body, now - 31);
        rejects([&] { server.open('M', expired, now); }, "expired");
        auto future = client.seal('M', body, now + 31);
        rejects([&] { server.open('M', future, now); }, "clock skew");
        rejects([&] { server.open('M', body, now); }, "payload length");
        // Fill and expire the bounded replay cache without evicting live nonces.
        control::Authenticator bounded(path, false);
        for (unsigned j = 0; j < 1024; j++)
            check(bounded.open('M', client.seal('M', body, now), now) == body,
                  "bounded nonce admission");
        rejects([&] { bounded.open('M', client.seal('M', body, now), now); }, "capacity");
        check(bounded.open('M', client.seal('M', body, now + 61), now + 61) == body,
              "expired nonce cache cleanup");
        auto before_restart = client.seal('M', body, now);
        control::Authenticator restarted(path, true);
        rejects([&] { restarted.open('M', before_restart, now); }, "authentication");
        control::Authenticator fresh_client(path, false);
        check(restarted.open('M', fresh_client.seal('M', body, now), now) == body,
              "restart key rotation and client reload");
#ifndef _WIN32
        check(::chmod(path.c_str(), 0644) == 0, "permission mutation");
        rejects([&] { control::Authenticator bad_permissions(path, false); }, "private");
#endif
        std::filesystem::remove_all(dir);
        std::cout << "PASS local control " << checks
                  << " checks (HMAC, replay, expiry, capacity, private secret)\n";
        return 0;
    } catch (const std::exception &e) {
        if (!dir.empty())
            std::filesystem::remove_all(dir);
        std::cerr << "FAIL control: " << e.what() << "\n";
        return 1;
    }
}
