#pragma once
#include "qbtc/consensus.hpp"
namespace qbtc::storage {
Bytes read_bounded(const std::string &path, std::size_t maximum);
void write_new(const std::string &path, std::span<const std::uint8_t> bytes);
void append_durable(const std::string &path, std::span<const std::uint8_t> bytes,
                    std::uint64_t observed_size, std::uint64_t committed_size);
void replace_atomic(const std::string &path, std::span<const std::uint8_t> bytes);
} // namespace qbtc::storage
