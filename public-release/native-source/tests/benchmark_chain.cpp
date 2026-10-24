#include "qbtc/consensus.hpp"
#include <charconv>
#include <chrono>
#include <iostream>
#ifndef _WIN32
#include <sys/resource.h>
#endif
int main(int argc, char **argv) {
    try {
        std::uint64_t count = 2016;
        if (argc == 2) {
            std::string_view s = argv[1];
            auto [p, e] = std::from_chars(s.data(), s.data() + s.size(), count);
            if (e != std::errc{} || p != s.data() + s.size() || count > 250000)
                throw qbtc::Invalid("benchmark count");
        }
        auto started = std::chrono::steady_clock::now();
        qbtc::Chain chain(qbtc::Network::Regtest);
        qbtc::Address address{};
        for (std::uint64_t j = 0; j < count; j++) {
            auto block =
                qbtc::candidate(chain, address, chain.tip().header.timestamp + qbtc::SPACING);
            qbtc::mine(block, 1000000);
            chain.add(block);
        }
        std::uint64_t live_value = 0;
        for (const auto &[id, coin] : chain.utxos()) {
            (void)id;
            live_value += coin.output.value;
        }
        if (count == 210001 && live_value != 10500000 * qbtc::COIN)
            throw qbtc::Invalid("first real halving chain monetary state");
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - started)
                      .count();
        std::cout << "height=" << chain.tip().header.height << " utxos=" << chain.utxos().size()
                  << " elapsed_ms=" << ms << " live_atoms=" << live_value;
#ifndef _WIN32
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        std::cout << " peak_rss_kib=" << usage.ru_maxrss;
#endif
#ifdef QBTC_DELTAS
        std::cout << " delta_records=" << chain.delta_records();
        if (chain.delta_records() != count)
            throw qbtc::Invalid("coinbase delta growth is not linear");
#endif
        std::cout << "\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
