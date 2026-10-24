#pragma once
#include "qbtc/consensus.hpp"
#include <algorithm>
#include <chrono>
namespace qbtc {
// Local admission policy, owned by the node thread. Never used in block consensus.
class RequestBudget {
    using Clock = std::chrono::steady_clock;
    double capacity_, rate_, available_;
    Clock::time_point updated_;
  public:
    RequestBudget(unsigned capacity, unsigned per_second, Clock::time_point now = Clock::now())
        : capacity_(capacity), rate_(per_second), available_(capacity), updated_(now) {
        if (!capacity || !per_second) throw Invalid("invalid request budget");
    }
    bool take(Clock::time_point now = Clock::now()) {
        if (now > updated_) {
            available_ = std::min(capacity_, available_ +
                std::chrono::duration<double>(now - updated_).count() * rate_);
            updated_ = now;
        }
        if (available_ < 1) return false;
        available_ -= 1;
        return true;
    }
};
}
