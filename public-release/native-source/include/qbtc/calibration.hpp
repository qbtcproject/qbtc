#pragma once
#include "qbtc/miner.hpp"
#include <chrono>

namespace qbtc {
struct Calibration {
    std::uint64_t hashes, elapsed_us;
    unsigned threads;
};
inline Calibration calibrate_cpu(std::string_view mode, unsigned seconds) {
    if (seconds < 2 || seconds > 120) throw Invalid("benchmark seconds must be 2..120");
    auto threads = mining_threads(mode);
    Header header;
    header.network = Network::Mainnet;
    header.target.fill(0);
    header.target.back() = 1;
    // Deliberately unattached header. No Chain, wallet, storage or network is used.
    {
        MiningJob warmup(header, threads);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        warmup.stop();
        (void)warmup.result();
    }
    auto started = std::chrono::steady_clock::now();
    MiningJob job(header, threads);
    std::this_thread::sleep_until(started + std::chrono::seconds(seconds));
    job.stop();
    (void)job.result();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
    if (elapsed <= 0) throw Invalid("benchmark clock");
    return {job.take_attempts(), static_cast<std::uint64_t>(elapsed), threads};
}
}
