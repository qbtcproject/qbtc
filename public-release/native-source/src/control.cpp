#include "qbtc/platform.hpp"
#include "qbtc/control.hpp"
#include "qbtc/storage.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace qbtc::control {
namespace {
void require(bool ok, const char *message) {
    if (!ok)
        throw Invalid(message);
}
Bytes authenticated(char command, std::span<const std::uint8_t> request) {
    Bytes message = {'Q', 'B', 'T', 'C', '/', 'l', 'o', 'c', 'a', 'l',
                     '-', 'c', 'o', 'n', 't', 'r', 'o', 'l', '/', '1'};
    auto id = blockid(genesis(ACTIVE_NETWORK).header);
    message.insert(message.end(), id.begin(), id.end());
    message.push_back(static_cast<std::uint8_t>(command));
    message.insert(message.end(), request.begin(), request.end());
    return message;
}
std::array<std::uint8_t, 32> mac(const std::array<std::uint8_t, 32> &key, char command,
                                 std::span<const std::uint8_t> request) {
    auto message = authenticated(command, request);
    std::array<std::uint8_t, 32> tag{};
    std::size_t n = 0;
    require(EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA3-256", nullptr, key.data(), key.size(),
                      message.data(), message.size(), tag.data(), tag.size(), &n) != nullptr &&
                n == 32,
            "control HMAC failure");
    return tag;
}
} // namespace
std::uint64_t unix_seconds() {
    auto n = std::chrono::duration_cast<std::chrono::seconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count();
    require(n >= 0, "control clock before epoch");
    return static_cast<std::uint64_t>(n);
}
Authenticator::Authenticator(const std::string &path, bool create) {
    const bool exists = std::filesystem::exists(file_path(path));
    if (exists) {
#ifndef _WIN32
        struct stat info{};
        require(::lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
                    info.st_uid == ::geteuid() && (info.st_mode & 077) == 0,
                "control file must be owned by current user, regular and private");
#endif
    }
    if (create) {
        require(RAND_bytes(key_.data(), 32) == 1, "control secret randomness");
        // New server instance, new key: authenticated packets recorded before a
        // restart cannot bypass the in-memory replay cache after that restart.
        if (exists)
            storage::replace_atomic(path, key_);
        else
            storage::write_new(path, key_);
    } else {
        auto bytes = storage::read_bounded(path, 32);
        require(bytes.size() == 32, "control secret length");
        std::copy(bytes.begin(), bytes.end(), key_.begin());
        OPENSSL_cleanse(bytes.data(), bytes.size());
    }
}

Authenticator::~Authenticator() {
    OPENSSL_cleanse(key_.data(), key_.size());
}
Bytes Authenticator::seal(char command, std::span<const std::uint8_t> body,
                          std::uint64_t now) const {
    require(body.size() <= 256, "control body limit");
    Bytes payload(40);
    require(RAND_bytes(payload.data(), 32) == 1, "control nonce randomness");
    for (unsigned j = 0; j < 8; j++)
        payload[32 + j] = static_cast<std::uint8_t>(now >> (8 * j));
    payload.insert(payload.end(), body.begin(), body.end());
    auto tag = mac(key_, command, payload);
    payload.insert(payload.end(), tag.begin(), tag.end());
    return payload;
}
Bytes Authenticator::open(char command, std::span<const std::uint8_t> payload, std::uint64_t now) {
    require(payload.size() >= 72 && payload.size() <= 328, "authenticated control payload length");
    auto request = payload.first(payload.size() - 32);
    auto tag = mac(key_, command, request);
    require(CRYPTO_memcmp(tag.data(), payload.data() + payload.size() - 32, 32) == 0,
            "control authentication failed");
    std::uint64_t sent = 0;
    for (unsigned j = 0; j < 8; j++)
        sent |= std::uint64_t(payload[32 + j]) << (8 * j);
    require(sent <= now ? (now - sent <= 30) : (sent - now <= 30),
            "control request expired or clock skew");
    std::array<std::uint8_t, 32> nonce{};
    std::copy_n(payload.begin(), 32, nonce.begin());
    for (auto it = used_.begin(); it != used_.end();)
        if (now >= it->second && now - it->second > 60)
            it = used_.erase(it);
        else
            ++it;
    require(!used_.contains(nonce), "control replay rejected");
    require(used_.size() < 1024, "control replay-cache capacity");
    used_.emplace(nonce, now);
    return {payload.begin() + 40, payload.end() - 32};
}
} // namespace qbtc::control
