#pragma once
#include "qbtc/consensus.hpp"
#include <functional>

namespace qbtc {
// Sparse overlay: reads through to a stable parent, records only changed coins.
// Parent and overlay must remain alive until validation/commit completes.
class CoinsView {
    std::function<const Coin *(const Outpoint &)> base_;
    std::map<Outpoint, std::optional<Coin>> changes_;

  public:
    explicit CoinsView(const Utxos &base);
    explicit CoinsView(std::function<const Coin *(const Outpoint &)> base);
    const Coin *get(const Outpoint &key) const;
    void set(const Outpoint &key, std::optional<Coin> value);
    UtxoDelta delta() const;
    void apply(const UtxoDelta &delta, bool forward);
};
struct TransactionResult {
    UtxoDelta delta;
    std::uint64_t fees = 0;
};
TransactionResult validate_transactions(const Block &block, const CoinsView &parent);
// Allocate all new map nodes before changing the live state. Once prepared,
// erase/merge uses nonthrowing key comparison and no further allocation.
Utxos prepare_commit(const UtxoDelta &delta);
void commit_prepared(Utxos &state, const UtxoDelta &delta, Utxos &prepared);
} // namespace qbtc
