#include "qbtc/consensus.hpp"
#include <iostream>
using namespace qbtc;
void check(bool ok, const char *message) { if (!ok) throw Invalid(message); }
int main() {
    auto limit = pow_limit(Network::Mainnet);
    constexpr auto period = INTERVAL * SPACING;
    check(retarget(limit, 0, period, Network::Mainnet) == limit, "stable target");
    auto harder = retarget(limit, 0, 1, Network::Mainnet);
    check(harder == retarget(limit, 0, period / 4, Network::Mainnet), "fourfold increase clamp");
    check(retarget(limit, 0, period * 100, Network::Mainnet) == limit, "decrease clamp and pow limit");
    check(work(harder) > work(limit), "work inverse to target");
    Chain slow(Network::Regtest), fast(Network::Regtest), combined(Network::Regtest);
    for (unsigned i = 0; i < 2018; ++i) {
        auto b = candidate(slow, Address{}, slow.tip_header().timestamp + 600);
        (void)mine(b, 100000); slow.add(b); combined.add(b);
    }
    for (unsigned i = 0; i < 2016; ++i) {
        auto b = candidate(fast, Address{}, fast.tip_header().timestamp + 1);
        (void)mine(b, 100000); fast.add(b); combined.add(b);
    }
    check(combined.tip_header().height == 2016 &&
          blockid(combined.tip_header()) == blockid(fast.tip_header()), "shorter higher-work branch wins");
    auto tip = blockid(combined.tip_header()); auto size = combined.utxos().size();
    auto bad = candidate(combined, Address{}, combined.tip_header().timestamp + 1);
    bad.header.target = pow_limit(Network::Regtest);
    (void)mine(bad, 100000);
    check(valid_pow(bad.header), "invalid target has valid own proof");
    bool rejected = false;
    try { combined.add(bad); } catch (const Invalid &e) { rejected = std::string(e.what()) == "unexpected target"; }
    check(rejected && blockid(combined.tip_header()) == tip && combined.utxos().size() == size,
          "wrong target rejected without mutation");
    std::cout << "PASS 2016-block integration, factor-four retarget, wrong-target rejection, shorter higher-work chain; disposable regtest only\n";
}
