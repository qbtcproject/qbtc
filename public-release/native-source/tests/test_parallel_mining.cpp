#include "qbtc/miner.hpp"
#include "qbtc/address.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace qbtc;
void check(bool ok, const char *reason) { if (!ok) throw std::runtime_error(reason); }
int main() {
    check(mining_threads("low", 16) == 1, "low mode");
    check(mining_threads("medium", 16) == 8, "medium mode");
    check(mining_threads("high", 16) == 15, "high mode");
    check(mining_threads("high", 0) == 1 && mining_threads("medium", 1) == 1, "single CPU fallback");
    bool invalid = false;
    try { (void)mining_threads("invalid"); } catch (const Invalid &) { invalid = true; }
    check(invalid, "invalid mode rejected");
    for (unsigned workers : {1u, 4u}) {
        Chain chain(Network::Regtest);
        for (unsigned height = 1; height <= 10; ++height) {
            auto block = candidate(chain, Address{}, chain.tip_header().timestamp + SPACING, {});
            MiningJob job(block.header, workers);
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            std::optional<Header> result;
            while (!(result = job.result()) && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            check(bool(result), "workers find a result");
            job.stop();
            check(job.take_attempts() > 0, "attempts recorded");
            block.header = *result;
            chain.add(block);
            check(chain.tip_header().height == height, "worker block accepted by independent consensus");
        }
    }
    auto hard = genesis(Network::Regtest).header;
    hard.network = Network::Mainnet; // Hard work fixture; no activated mainnet chain required.
    hard.target.fill(0); hard.target.back() = 1;
    std::uint64_t resume = 0;
    for (unsigned workers : {1u, 4u, 1u}) {
        hard.nonce = resume;
        MiningJob job(hard, workers);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto started = std::chrono::steady_clock::now();
        job.stop();
        check(std::chrono::steady_clock::now() - started < std::chrono::seconds(2), "bounded cancellation");
        check(!job.result(), "hard work not spuriously solved");
        check(job.take_attempts() > 0 && job.next_nonce() > resume, "mode switch retains forward nonce progress");
        resume = job.next_nonce();
    }
    hard.nonce = std::numeric_limits<std::uint64_t>::max() - 10;
    MiningJob exhausted(hard, 4);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    exhausted.stop();
    invalid = false;
    try { (void)exhausted.result(); } catch (const Invalid &) { invalid = true; }
    check(invalid && exhausted.next_nonce() == std::numeric_limits<std::uint64_t>::max(), "nonce exhaustion cannot wrap");
    std::cout << "fixture_address=" << wallet::address(Address{}) << "\n";
    std::cout << "PASS parallel mining: mode mapping, consensus-accepted blocks, cancellation, forward progress, nonce exhaustion\n";
}
