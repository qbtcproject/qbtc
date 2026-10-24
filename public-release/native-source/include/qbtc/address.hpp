#pragma once
#include "qbtc/consensus.hpp"
namespace qbtc::wallet {
std::string address(const Address &owner, Network network = ACTIVE_NETWORK);
Address parse_address(std::string_view text, Network network = ACTIVE_NETWORK);
} // namespace qbtc::wallet
