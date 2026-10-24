#pragma once
#include "qbtc/consensus.hpp"
#include <array>

namespace qbtc::control {
// Local control secret, unrelated to funds or consensus. Never sent in a frame.
class Authenticator {
    std::array<std::uint8_t, 32> key_{};
    std::map<std::array<std::uint8_t, 32>, std::uint64_t> used_;

  public:
    explicit Authenticator(const std::string &path, bool create);
    ~Authenticator();
    Authenticator(const Authenticator &) = delete;
    Authenticator &operator=(const Authenticator &) = delete;
    Bytes seal(char command, std::span<const std::uint8_t> body, std::uint64_t now) const;
    Bytes open(char command, std::span<const std::uint8_t> payload, std::uint64_t now);
};
std::uint64_t unix_seconds();
} // namespace qbtc::control
