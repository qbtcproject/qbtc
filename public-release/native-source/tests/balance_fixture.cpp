#include "qbtc/wallet.hpp"
#include <filesystem>
#include <iostream>
using namespace qbtc;
int main(int argc, char **argv) {
    if (argc != 2) return 1;
    std::filesystem::path dir(argv[1]);
    auto key = wallet::Key::generate();
    auto owner = key.owner();
    Address recipient{};
    Chain chain(Network::Regtest);
    for (unsigned i = 0; i < 101; ++i) {
        auto b = candidate(chain, owner, chain.tip_header().timestamp + SPACING);
        (void)mine(b, 1000000); chain.add(b);
    }
    chain.append((dir / "before.db").string());
    auto tx = wallet::payment(key, chain, recipient, 2500000000ULL, 1000);
    auto block = candidate(chain, owner, chain.tip_header().timestamp + SPACING, {tx});
    (void)mine(block, 1000000); chain.add(block);
    chain.append((dir / "after.db").string());
    Chain mainnet(Network::Mainnet);
    mainnet.append((dir / "mainnet.db").string());
    std::cout << wallet::address(owner, Network::Regtest) << "\n"
              << wallet::address(recipient, Network::Regtest) << "\n"
              << wallet::address(recipient, Network::Mainnet) << "\n";
}
