#include "qbtc/consensus.hpp"
#include "qbtc/storage.hpp"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <openssl/rand.h>
using namespace qbtc;
namespace {
int checks = 0;
void check(bool value, const char *text) {
    if (!value)
        throw std::runtime_error(text);
    checks++;
}
void rejects(const std::function<void()> &f, const char *expected) {
    try {
        f();
    } catch (const Invalid &e) {
        check(std::string(e.what()).find(expected) != std::string::npos, e.what());
        return;
    }
    throw std::runtime_error(expected);
}
Block next(Chain &chain, std::uint8_t owner) {
    Address a{};
    a[0] = owner;
    auto b = candidate(chain, a, chain.tip().header.timestamp + SPACING);
    mine(b, 1000000);
    return b;
}
void replace(const std::string &path, const Bytes &bytes) {
    storage::replace_atomic(path, bytes);
}
} // namespace
int main() {
    std::filesystem::path dir;
    try {
        Bytes salt(8);
        check(RAND_bytes(salt.data(), 8) == 1, "temp randomness");
        dir = std::filesystem::temp_directory_path() / ("qbtc-journal-" + hex(salt));
        std::filesystem::create_directory(dir);
        auto path = (dir / "chain.journal").string();
        Chain chain(Network::Regtest);
        chain.append(path);
        check(Chain::load(Network::Regtest, path).utxos().empty(), "empty journal genesis");
        chain.add(next(chain, 1));
        chain.append(path);
        auto prefix = storage::read_bounded(path, 1024 * 1024);
        auto prefix_size = prefix.size();
        chain.add(next(chain, 1));
        chain.append(path);
        auto full = storage::read_bounded(path, 1024 * 1024);
        check(full.size() > prefix_size, "incremental file growth");
        check(std::equal(prefix.begin(), prefix.end(), full.begin()),
              "previous committed prefix unchanged");
        auto loaded = Chain::load(Network::Regtest, path);
        check(loaded.utxos() == chain.utxos(), "journal replay identical state");
        check(loaded.recovered_tail_bytes() == 0, "complete journal no tail");
        // Every truncation of the final frame must expose only the previous commit.
        auto torn = (dir / "torn.journal").string();
        for (std::size_t cut = prefix_size; cut < full.size(); cut++) {
            replace(torn, Bytes(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(cut)));
            auto recovered = Chain::load(Network::Regtest, torn);
            check(recovered.tip().header.height == 1, "torn frame does not expose partial blocks");
            check(recovered.recovered_tail_bytes() == cut - prefix_size, "tail length reported");
        }
        auto recovered = Chain::load(Network::Regtest, torn);
        recovered.add(next(recovered, 3));
        recovered.append(torn);
        auto fixed = Chain::load(Network::Regtest, torn);
        check(fixed.recovered_tail_bytes() == 0 && fixed.utxos() == recovered.utxos(),
              "truncate uncommitted tail before append");
        auto corrupted = full;
        corrupted[prefix_size + 20] ^= 1;
        replace(torn, corrupted);
        rejects([&] { Chain::load(Network::Regtest, torn); }, "checksum");
        corrupted = full;
        corrupted.back() ^= 1;
        replace(torn, corrupted);
        rejects([&] { Chain::load(Network::Regtest, torn); }, "commit marker");
        // Detect a concurrent external append rather than blindly truncating it.
        {
            std::ofstream out(path, std::ios::binary | std::ios::app);
            out.put('x');
        }
        auto externally_changed = storage::read_bounded(path, 1024 * 1024);
        loaded.add(next(loaded, 1));
        rejects([&] { loaded.append(path); }, "changed since load");
        check(storage::read_bounded(path, 1024 * 1024) == externally_changed,
              "external change rejection preserves every byte");
        // Snapshot v1 remains readable; migration to journal preserves coins and tip.
        auto snapshot = (dir / "legacy.db").string();
        chain.save(snapshot);
        auto migrating = Chain::load(Network::Regtest, snapshot);
        migrating.append(snapshot);
        check(Chain::load(Network::Regtest, snapshot).utxos() == chain.utxos(),
              "snapshot to journal migration");
        // Tie restoration must move active UTXOs too, not only change the tip hash.
        Chain first(Network::Regtest);
        auto a = next(first, 4), b = next(first, 5);
        auto aid = blockid(a.header), bid = blockid(b.header);
        if (aid < bid) {
            first.add(b);
            first.add(a);
        } else {
            first.add(a);
            first.add(b);
        }
        auto equal = (dir / "tie.db").string();
        first.save(equal);
        auto equal_loaded = Chain::load(Network::Regtest, equal);
        check(blockid(equal_loaded.tip().header) == blockid(first.tip().header) &&
                  equal_loaded.utxos() == first.utxos(),
              "equal-work saved tip and UTXOs restored consistently");
        // A local budget is not consensus: the same valid block passes a node with
        // sufficient resources, and a budget failure leaves the constrained node intact.
        ResourceBudget small;
        small.blocks = 1;
        Chain constrained(Network::Regtest, small), unconstrained(Network::Regtest);
        auto one = next(constrained, 1);
        constrained.add(one);
        unconstrained.add(one);
        auto two = next(constrained, 1);
        bool exhausted = false;
        try {
            constrained.add(two);
        } catch (const ResourceLimit &) {
            exhausted = true;
        }
        check(exhausted && constrained.tip().header.height == 1,
              "resource exhaustion preserves state");
        unconstrained.add(two);
        check(unconstrained.tip().header.height == 2,
              "resource policy does not change consensus validity");
        // Deep branch transitions exercise reversible deltas without state snapshots.
        Chain main(Network::Regtest), fork(Network::Regtest);
        for (unsigned j = 0; j < 250; j++) {
            auto block = next(main, 1);
            main.add(block);
            if (j < 100)
                fork.add(block);
        }
        for (unsigned j = 100; j < 260; j++) {
            auto block = next(fork, 2);
            fork.add(block);
            main.add(block);
        }
        check(main.utxos() == fork.utxos(), "deep reorg state matches independent branch");
        check(main.delta_records() == 410, "one delta per retained coinbase block");
        check(main.utxos().size() == 260, "deep reorg active supply count");
        // Committed bodies live on disk; all branches remain accessible after eviction.
        auto disk_path = (dir / "bodies.journal").string();
        main.append(disk_path);
        check(main.resident_block_bytes() == 0, "commit releases transaction bodies");
        auto disk_loaded = Chain::load(Network::Regtest, disk_path);
        check(disk_loaded.resident_block_bytes() == 0, "replay releases bodies incrementally");
        check(disk_loaded.utxos() == main.utxos(), "disk backed reorg state");
        auto cursor = blockid(main.tip().header);
        for (unsigned j = 0; j < 260; j++) {
            auto body = disk_loaded.block(cursor);
            check(encode(body) == encode(main.block(cursor)), "indexed body round trip");
            cursor = body.header.previous;
        }
        auto moved_path = (dir / "copied.journal").string();
        disk_loaded.append(moved_path);
        check(disk_loaded.resident_block_bytes() == 0, "new journal keeps bodies evicted");
        std::filesystem::remove(disk_path);
        check(disk_loaded.tip().header.height == 260, "new store offsets replace old offsets");
        auto snapshot_copy = (dir / "export.db").string();
        disk_loaded.save(snapshot_copy);
        check(Chain::load(Network::Regtest, snapshot_copy).utxos() == disk_loaded.utxos(),
              "snapshot export reads indexed bodies");
        rejects([&] { disk_loaded.save(moved_path); }, "active body store");
        // A small resident-body budget can accept many committed blocks.
        ResourceBudget streaming_budget;
        streaming_budget.block_bytes = 1024;
        Chain streaming(Network::Regtest, streaming_budget);
        auto stream_path = (dir / "stream.journal").string();
        streaming.append(stream_path);
        Hash original_first_id{};
        for (unsigned j = 0; j < 50; j++) {
            auto streamed_id = streaming.add(next(streaming, 7));
            if (j == 0)
                original_first_id = streamed_id;
            check(streaming.resident_block_bytes() > 0, "uncommitted body remains resident");
            streaming.append(stream_path);
            check(streaming.resident_block_bytes() == 0, "durable append evicts body");
        }
        // Reorganize when both the detached and attaching branch bodies were evicted.
        Chain branch(Network::Regtest);
        for (unsigned j = 0; j < 51; j++) {
            auto body = next(branch, 8);
            branch.add(body);
            streaming.add(body);
            streaming.append(stream_path);
        }
        check(streaming.utxos() == branch.utxos(), "reorg after body eviction");
        // Detect edits to an already-loaded body, even without replaying the journal.
        auto stream_bytes = storage::read_bounded(stream_path, 1024 * 1024);
        auto damaged = stream_bytes;
        // First block starts after header, initial empty frame, frame length and payload prefix.
        auto first_body = std::size_t(73 + 208 + 4 + 132 + 4);
        damaged.at(first_body + 224) ^= 1;
        replace(stream_path, damaged);
        rejects([&] { (void)streaming.block(original_first_id); }, "checksum");
        std::filesystem::remove_all(dir);
        std::cout << "PASS journal/undo " << checks
                  << " checks (torn writes, checksum, migration, equal-work restore, deep reorg)\n";
        return 0;
    } catch (const std::exception &e) {
        if (!dir.empty())
            std::filesystem::remove_all(dir);
        std::cerr << "FAIL journal: " << e.what() << " after " << checks << " checks\n";
        return 1;
    }
}
