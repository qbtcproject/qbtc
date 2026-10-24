#pragma once
#include "qbtc/consensus.hpp"
#include <algorithm>
#include <atomic>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

namespace qbtc {
inline unsigned mining_threads(std::string_view mode, unsigned available = std::thread::hardware_concurrency()) {
    available = std::clamp(available, 1u, 256u);
    if (mode == "low") return 1;
    if (mode == "medium") return std::max(1u, available / 2);
    if (mode == "high") return std::max(1u, available - 1);
    throw Invalid("mining mode must be low, medium or high");
}
// Workers see only a copied header. The node thread alone validates and commits blocks.
class MiningJob {
    std::atomic<bool> stopped_{false};
    std::atomic<std::uint64_t> next_, attempts_{0};
    std::mutex result_mutex_;
    std::optional<Header> result_;
    std::exception_ptr error_;
    std::vector<std::thread> workers_;
    void run(Header h) noexcept {
        try {
            while (!stopped_.load(std::memory_order_relaxed)) {
                auto first = next_.load(std::memory_order_relaxed);
                std::uint64_t count;
                do {
                    if (first == std::numeric_limits<std::uint64_t>::max())
                        throw Invalid("mining nonce exhausted");
                    count = std::min(std::uint64_t{1024}, std::numeric_limits<std::uint64_t>::max() - first);
                } while (!next_.compare_exchange_weak(first, first + count, std::memory_order_relaxed));
                std::uint64_t tried = 0;
                for (; tried < count && !stopped_.load(std::memory_order_relaxed); ++tried) {
                    h.nonce = first + tried;
                    if (valid_pow(h)) {
                        attempts_.fetch_add(tried + 1, std::memory_order_relaxed);
                        std::lock_guard lock(result_mutex_);
                        if (!result_) result_ = h;
                        stopped_.store(true, std::memory_order_relaxed);
                        return;
                    }
                }
                attempts_.fetch_add(tried, std::memory_order_relaxed);
            }
        } catch (...) {
            std::lock_guard lock(result_mutex_);
            if (!error_) error_ = std::current_exception();
            stopped_.store(true, std::memory_order_relaxed);
        }
    }
  public:
    MiningJob(Header header, unsigned threads) : next_(header.nonce) {
        if (!threads || threads > 256) throw Invalid("mining worker limit");
        try {
            for (unsigned i = 0; i < threads; ++i)
                workers_.emplace_back([this, header] { run(header); });
        } catch (...) { stop(); throw; }
    }
    MiningJob(const MiningJob &) = delete;
    ~MiningJob() { stop(); }
    void stop() noexcept {
        stopped_.store(true, std::memory_order_relaxed);
        for (auto &worker : workers_) if (worker.joinable()) worker.join();
    }
    std::uint64_t take_attempts() { return attempts_.exchange(0, std::memory_order_relaxed); }
    std::uint64_t next_nonce() const { return next_.load(std::memory_order_relaxed); }
    std::optional<Header> result() {
        std::lock_guard lock(result_mutex_);
        if (error_) std::rethrow_exception(error_);
        return result_;
    }
};
}
