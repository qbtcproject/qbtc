#include "qbtc/mempool.hpp"
#include "qbtc/storage.hpp"
#include <algorithm>
#include <filesystem>
#include <functional>
#include <iostream>
#include <openssl/rand.h>
using namespace qbtc;
namespace {
unsigned checks = 0;
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
    checks++;
}
void rejects(const std::function<void()> &action) {
    try {
        action();
    } catch (const Invalid &) {
        checks++;
        return;
    }
    throw std::runtime_error("invalid mempool file accepted");
}
void seal(Bytes &bytes) {
    auto digest = hash(std::span<const std::uint8_t>(bytes).first(bytes.size() - 64));
    std::copy(digest.begin(), digest.end(), bytes.end() - 64);
}
Transaction sample() {
    Transaction tx;
    Input in;
    in.previous.txid[0] = 7;
    tx.inputs.push_back(in);
    tx.outputs.push_back({1, {}});
    return tx;
}
} // namespace
int main() {
    std::filesystem::path directory;
    try {
        auto tx = sample();
        auto empty = mempool::encode_file(Network::Regtest, {});
        check(empty.size() == 141, "empty file canonical size");
        check(mempool::decode_file(Network::Regtest, empty).empty(), "empty round trip");
        auto bytes = mempool::encode_file(Network::Regtest, {tx});
        auto restored = mempool::decode_file(Network::Regtest, bytes);
        check(restored.size() == 1 && encode(restored[0]) == encode(tx), "transaction round trip");
        for (std::size_t cut = 0; cut < bytes.size(); cut++)
            rejects([&] {
                (void)mempool::decode_file(Network::Regtest,
                                           std::span<const std::uint8_t>(bytes).first(cut));
            });
        for (std::size_t index = 0; index < bytes.size(); index++) {
            auto bad = bytes;
            bad[index] ^= 1;
            rejects([&] { (void)mempool::decode_file(Network::Regtest, bad); });
        }
        auto bad = bytes;
        bad[0] ^= 1;
        seal(bad);
        rejects([&] { (void)mempool::decode_file(Network::Regtest, bad); });
        bad = bytes;
        bad[5] ^= 1;
        seal(bad);
        rejects([&] { (void)mempool::decode_file(Network::Regtest, bad); });
        bad = bytes;
        bad[9] ^= 1;
        seal(bad);
        rejects([&] { (void)mempool::decode_file(Network::Regtest, bad); });
        bad = bytes;
        bad[73] = 65;
        seal(bad);
        rejects([&] { (void)mempool::decode_file(Network::Regtest, bad); });
        bad = bytes;
        bad[77] = 255;
        bad[78] = 255;
        seal(bad);
        rejects([&] { (void)mempool::decode_file(Network::Regtest, bad); });
        bad = bytes;
        bad.insert(bad.end() - 64, 0);
        seal(bad);
        rejects([&] { (void)mempool::decode_file(Network::Regtest, bad); });
        rejects([&] { (void)mempool::encode_file(Network::Regtest, {tx, tx}); });
        auto coinbase = tx;
        coinbase.coinbase_height = 1;
        rejects([&] { (void)mempool::encode_file(Network::Regtest, {coinbase}); });
        rejects([&] {
            (void)mempool::encode_file(Network::Regtest, std::vector<Transaction>(65, tx));
        });
        // Checksummed files still reject duplicate transaction records.
        bad = bytes;
        auto record = Bytes(bytes.begin() + 77, bytes.end() - 64);
        bad.insert(bad.end() - 64, record.begin(), record.end());
        bad[73] = 2;
        seal(bad);
        rejects([&] { (void)mempool::decode_file(Network::Regtest, bad); });
        Bytes salt(8);
        check(RAND_bytes(salt.data(), 8) == 1, "random temporary name");
        directory = std::filesystem::temp_directory_path() / ("qbtc-mempool-" + hex(salt));
        std::filesystem::create_directory(directory);
        auto path = (directory / "pending").string();
        mempool::save(path, Network::Regtest, {tx});
        check(mempool::load(path, Network::Regtest).size() == 1, "durable file round trip");
        rejects([&] { mempool::save(path, Network::Regtest, {tx, tx}); });
        check(mempool::load(path, Network::Regtest).size() == 1,
              "failed serialization preserves prior file");
        mempool::save(path, Network::Regtest, {});
        check(mempool::load(path, Network::Regtest).empty(),
              "atomic removal of pending transaction");
        storage::replace_atomic(path, bad);
        rejects([&] { (void)mempool::load(path, Network::Regtest); });
        check(storage::read_bounded(path, mempool::MAX_FILE_BYTES) == bad,
              "corruption preserved for recovery");
        std::filesystem::remove_all(directory);
        std::cout << "PASS mempool persistence " << checks << " checks\n";
        return 0;
    } catch (const std::exception &error) {
        if (!directory.empty())
            std::filesystem::remove_all(directory);
        std::cerr << "FAIL mempool: " << error.what() << "\n";
        return 1;
    }
}
