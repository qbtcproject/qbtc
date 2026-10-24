#include "qbtc/consensus.hpp"
#include <iostream>
#include <random>
using namespace qbtc;
int main() {
    try {
        Chain seed_chain(Network::Regtest);
        Address recipient{};
        auto seed = candidate(seed_chain, recipient, seed_chain.tip().header.timestamp + SPACING);
        mine(seed, 100000);
        auto original = encode(seed);
        std::mt19937 random(0x51425443);
        unsigned accepted = 0, rejected = 0, decoded = 0;
        for (unsigned iteration = 0; iteration < 20000; iteration++) {
            auto input = original;
            if (iteration % 100 != 0) {
                auto at = static_cast<std::size_t>(random()) % input.size();
                switch (random() % 6) {
                case 0:
                    input[at] ^= static_cast<std::uint8_t>(1U << (random() % 8));
                    break;
                case 1:
                    input.erase(input.begin() + static_cast<std::ptrdiff_t>(at));
                    break;
                case 2:
                    input.insert(input.begin() + static_cast<std::ptrdiff_t>(at),
                                 static_cast<std::uint8_t>(random()));
                    break;
                case 3:
                    input.resize(at);
                    break;
                case 4:
                    input.insert(input.end(), 32, 255);
                    break;
                default:
                    for (unsigned j = 0; j < 4; j++)
                        input[(at + j) % input.size()] = 255;
                    break;
                }
            }
            Chain chain(Network::Regtest);
            auto before = blockid(chain.tip().header);
            try {
                auto block = decode_block(input);
                if (encode(block) != input)
                    throw std::runtime_error("noncanonical decoded block");
                decoded++;
                chain.add(block);
                std::uint64_t supply = 0;
                for (const auto &[id, coin] : chain.utxos()) {
                    (void)id;
                    supply += coin.output.value;
                }
                if (chain.tip().header.height != 1 || supply > 50 * COIN)
                    throw std::runtime_error("mutated accepted block violates monetary state");
                accepted++;
            } catch (const Invalid &) {
                rejected++;
                if (blockid(chain.tip().header) != before || !chain.utxos().empty() ||
                    chain.delta_records() != 0)
                    throw std::runtime_error("malformed block mutated state");
            }
            try {
                auto tx = decode_transaction(input);
                if (encode(tx) != input)
                    throw std::runtime_error("noncanonical decoded transaction");
            } catch (const Invalid &) {
            }
        }
        if (accepted == 0 || rejected == 0 || decoded == 0)
            throw std::runtime_error("mutation campaign did not exercise both paths");
        std::cout << "PASS structured mutation campaign: 20000 inputs, " << decoded << " decoded, "
                  << accepted << " accepted valid blocks, " << rejected << " rejected atomically\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL mutation campaign: " << e.what() << "\n";
        return 1;
    }
}
