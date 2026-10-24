#include "qbtc/utxo.hpp"
#include <set>

namespace qbtc {
namespace {
void require(bool ok, const char *message) {
    if (!ok)
        throw Invalid(message);
}
std::optional<Coin> copy_coin(const Coin *coin) {
    return coin ? std::optional<Coin>(*coin) : std::nullopt;
}
std::uint64_t add_money(std::uint64_t a, std::uint64_t b) {
    require(a <= MAX_MONEY && b <= MAX_MONEY - a, "money range");
    return a + b;
}
} // namespace
CoinsView::CoinsView(const Utxos &base)
    : base_([&base](const Outpoint &key) -> const Coin * {
          auto it = base.find(key);
          return it == base.end() ? nullptr : &it->second;
      }) {}
CoinsView::CoinsView(std::function<const Coin *(const Outpoint &)> base) : base_(std::move(base)) {}
const Coin *CoinsView::get(const Outpoint &key) const {
    auto it = changes_.find(key);
    if (it != changes_.end())
        return it->second ? &*it->second : nullptr;
    return base_(key);
}
void CoinsView::set(const Outpoint &key, std::optional<Coin> value) {
    changes_.insert_or_assign(key, std::move(value));
}
UtxoDelta CoinsView::delta() const {
    UtxoDelta result;
    result.reserve(changes_.size());
    for (const auto &[key, after] : changes_) {
        auto before = copy_coin(base_(key));
        if (before != after)
            result.push_back({key, std::move(before), after});
    }
    return result;
}
void CoinsView::apply(const UtxoDelta &delta, bool forward) {
    for (const auto &change : delta) {
        const auto &expected = forward ? change.before : change.after;
        require(copy_coin(get(change.outpoint)) == expected, "undo/delta state mismatch");
        set(change.outpoint, forward ? change.after : change.before);
    }
}
Utxos prepare_commit(const UtxoDelta &delta) {
    Utxos prepared;
    for (const auto &change : delta)
        if (change.after)
            prepared.emplace(change.outpoint, *change.after);
    return prepared;
}
void commit_prepared(Utxos &state, const UtxoDelta &delta, Utxos &prepared) {
    for (const auto &change : delta)
        state.erase(change.outpoint);
    state.merge(prepared);
}
TransactionResult validate_transactions(const Block &b, const CoinsView &parent) {
    require(b.header.height > 0 && !b.transactions.empty(), "missing coinbase");
    CoinsView state([&parent](const Outpoint &key) { return parent.get(key); });
    std::uint64_t fees = 0, reward = 0;
    std::set<Hash> ids;
    for (std::size_t ti = 0; ti < b.transactions.size(); ti++) {
        const auto &t = b.transactions[ti];
        require(t.version == 1 && (ti == 0 || !t.outputs.empty()),
                "transaction version or empty outputs");
        require(t.inputs.size() <= MAX_ITEMS && t.outputs.size() <= MAX_ITEMS,
                "transaction limits");
        auto id = txid(t);
        require(ids.insert(id).second, "duplicate transaction");
        const bool cb = ti == 0;
        require(cb ? (t.inputs.empty() && t.coinbase_height == b.header.height)
                   : (!t.inputs.empty() && t.coinbase_height == 0),
                "coinbase structure");
        std::uint64_t in = 0, out = 0;
        for (const auto &o : t.outputs) {
            require(o.value > 0, "zero output");
            out = add_money(out, o.value);
        }
        if (!cb) {
            std::set<Outpoint> spent;
            for (const auto &input : t.inputs)
                require(spent.insert(input.previous).second, "double spend");
            for (std::size_t j = 0; j < t.inputs.size(); j++) {
                const auto &i = t.inputs[j];
                const auto *coin = state.get(i.previous);
                require(coin != nullptr, "missing UTXO");
                require(!coin->coinbase || (b.header.height >= coin->height &&
                                            b.header.height - coin->height >= MATURITY),
                        "immature coinbase");
                require(key_address(i.public_key) == coin->output.owner, "wrong owner");
                require(
                    verify_signature(i.public_key,
                                     signature_message(b.header.network, t,
                                                       static_cast<std::uint32_t>(j), coin->output),
                                     i.signature),
                    "invalid or unavailable ML-DSA signature");
                in = add_money(in, coin->output.value);
            }
            require(in >= out, "overspend");
            fees = add_money(fees, in - out);
            for (const auto &i : t.inputs)
                state.set(i.previous, std::nullopt);
        } else
            reward = out;
        for (std::size_t j = 0; j < t.outputs.size(); j++) {
            Outpoint point{id, static_cast<std::uint32_t>(j)};
            require(state.get(point) == nullptr, "UTXO overwrite");
            state.set(point, Coin{t.outputs[j], b.header.height, cb});
        }
    }
    require(reward <= add_money(subsidy(b.header.height), fees), "excess coinbase reward");
    return {state.delta(), fees};
}
Utxos apply_transactions(const Block &b, const Utxos &old) {
    CoinsView view(old);
    auto result = validate_transactions(b, view);
    auto state = old;
    auto prepared = prepare_commit(result.delta);
    commit_prepared(state, result.delta, prepared);
    return state;
}
} // namespace qbtc
