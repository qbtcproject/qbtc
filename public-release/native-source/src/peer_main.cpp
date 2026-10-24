#include "qbtc/platform.hpp"
#include "qbtc/address.hpp"
#include "qbtc/consensus.hpp"
#include "qbtc/control.hpp"
#include "qbtc/lock.hpp"
#include "qbtc/mempool.hpp"
#include "qbtc/miner.hpp"
#include "qbtc/ratelimit.hpp"
#include "qbtc/calibration.hpp"
#include "qbtc/net.hpp"
#include "qbtc/peers.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <thread>
#include <tuple>
using namespace qbtc;
using namespace qbtc::net;
namespace {
void need(bool ok, const char *msg) {
    if (!ok)
        throw Invalid(msg);
}
void put32(Bytes &b, std::uint32_t n) {
    for (unsigned j = 0; j < 4; j++)
        b.push_back(static_cast<std::uint8_t>(n >> (8 * j)));
}
struct Reader {
    std::span<const std::uint8_t> data;
    std::size_t pos = 0;
    std::span<const std::uint8_t> take(std::size_t n) {
        need(n <= data.size() - pos, "truncated network payload");
        auto r = data.subspan(pos, n);
        pos += n;
        return r;
    }
    std::uint32_t u32() {
        auto v = take(4);
        std::uint32_t n = 0;
        for (unsigned j = 0; j < 4; j++)
            n |= std::uint32_t(v[j]) << (8 * j);
        return n;
    }
    Hash id() {
        auto b = take(64);
        Hash h;
        std::copy(b.begin(), b.end(), h.begin());
        return h;
    }
    void end() {
        need(pos == data.size(), "trailing network payload");
    }
};
std::vector<Hash> active(const Chain &chain) {
    std::vector<Hash> result;
    auto cursor = blockid(chain.tip_header());
    for (;;) {
        const auto &b = chain.header(cursor);
        result.push_back(cursor);
        if (b.height == 0)
            break;
        cursor = b.previous;
    }
    std::reverse(result.begin(), result.end());
    return result;
}
struct FatalStorage : std::runtime_error {
    using std::runtime_error::runtime_error;
};
class Node {
    Chain chain_;
    RequestBudget transaction_budget_{8, 2};
    RequestBudget read_budget_{32, 16};
    std::string path_;
    std::vector<Transaction> pool_;
    std::set<Hash> pool_ids_;
    std::size_t pool_bytes_ = 0;
    mutable Hash active_cache_tip_{};
    mutable bool active_cached_ = false;
    mutable std::vector<Hash> active_cache_;
    mutable std::map<Hash, std::size_t> active_positions_;
    const std::vector<Hash> &active_blocks() const {
        const auto tip = blockid(chain_.tip_header());
        if (!active_cached_ || active_cache_tip_ != tip) {
            auto blocks = active(chain_);
            std::map<Hash, std::size_t> positions;
            for (std::size_t i = 0; i < blocks.size(); ++i) positions.emplace(blocks[i], i);
            active_cache_.swap(blocks);
            active_positions_.swap(positions);
            active_cache_tip_ = tip;
            active_cached_ = true;
        }
        return active_cache_;
    }
    std::uint64_t last_hashes_ = 0, last_hps_ = 0;
    std::optional<Address> mining_owner_;
    std::optional<Block> mining_work_;
    std::optional<std::uint64_t> mining_remaining_;
    std::unique_ptr<MiningJob> mining_job_;
    std::string mining_mode_ = "medium";
    unsigned mining_threads_ = mining_threads("medium");
    void stop_mining_job() {
        if (!mining_job_) return;
        mining_job_->stop();
        last_hashes_ += mining_job_->take_attempts();
        if (mining_work_) mining_work_->header.nonce = mining_job_->next_nonce();
        mining_job_.reset();
    }
    Hash status_tip_{}, status_utxo_hash_{};
    bool status_cached_ = false;
    std::chrono::steady_clock::time_point mining_started_{};
    void update_mining_rate() {
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - mining_started_).count();
        if (elapsed > 0) last_hps_ = last_hashes_ * 1000000 / static_cast<std::uint64_t>(elapsed);
    }

  public:
    explicit Node(std::string path)
        : chain_(std::filesystem::exists(file_path(path)) ? Chain::load(ACTIVE_NETWORK, path)
                                               : Chain(ACTIVE_NETWORK)),
          path_(std::move(path)) {
        auto pool_path = path_ + ".mempool";
        if (std::filesystem::exists(file_path(pool_path))) {
            // File integrity errors stop startup without overwriting the original.
            auto pending = mempool::load(pool_path, ACTIVE_NETWORK);
            for (const auto &tx : pending)
                try {
                    admit(tx, false);
                } catch (const Invalid &) {
                }
        }
        persist();
    }
    void enable_mining(Address owner, std::optional<std::uint64_t> count) {
        mining_owner_ = owner;
        mining_remaining_ = count;
        mining_started_ = std::chrono::steady_clock::now();
        last_hashes_ = 0;
        last_hps_ = 0;
    }
    std::uint64_t mining_timestamp() const {
        if (ACTIVE_NETWORK == Network::Regtest) return chain_.tip_header().timestamp + SPACING;
        return std::max(control::unix_seconds(), chain_.tip_header().timestamp + 1);
    }
    void set_mining_mode(std::string_view mode) {
        auto threads = mining_threads(mode);
        if (mode == mining_mode_) return;
        stop_mining_job();
        mining_mode_ = mode;
        mining_threads_ = threads;
        last_hashes_ = last_hps_ = 0;
        mining_started_ = std::chrono::steady_clock::now();
    }
    void mine_step() {
        if (!mining_owner_) return;
        if (ACTIVE_NETWORK == Network::Mainnet && mining_timestamp() > control::unix_seconds() + 7200) {
            stop_mining_job();
            return;
        }
        if (!mining_work_ || mining_work_->header.previous != blockid(chain_.tip_header())) {
            stop_mining_job();
            mining_work_ = candidate(chain_, *mining_owner_, mining_timestamp(), pool_);
        }
        try {
            if (!mining_job_)
                mining_job_ = std::make_unique<MiningJob>(mining_work_->header, mining_threads_);
            last_hashes_ += mining_job_->take_attempts();
            auto solved = mining_job_->result();
            if (solved) {
                stop_mining_job();
                mining_work_->header = *solved;
                chain_.add(*mining_work_);
                mining_work_.reset();
                prune();
                persist();
                if (mining_remaining_ && --*mining_remaining_ == 0) mining_owner_.reset();
            }
            update_mining_rate();
        } catch (const ResourceLimit &error) {
            stop_mining_job();
            mining_owner_.reset();
            mining_work_.reset();
            std::cerr << "Local mining paused: " << error.what() << "\n";
        }
    }
    const Chain &chain() const {
        return chain_;
    }
    void persist_pool(const std::vector<Transaction> &transactions) {
        try {
            mempool::save(path_ + ".mempool", ACTIVE_NETWORK, transactions);
        } catch (const std::exception &e) {
            throw FatalStorage(std::string("mempool durable storage failure; node stopped: ") +
                               e.what());
        }
    }
    void admit(const Transaction &tx, bool durable = true) {
        mempool::check_admission(tx);
        auto id = txid(tx);
        if (pool_ids_.contains(id)) return;
        if (durable) need(transaction_budget_.take(), "transaction admission rate limit; retry later");
        auto size = encode(tx).size();
        need(pool_.size() < 64 && size <= MAX_BLOCK_BYTES - pool_bytes_, "mempool capacity");
        auto proposal = pool_;
        auto proposed_ids = pool_ids_;
        proposed_ids.insert(id);
        proposal.push_back(tx);
        (void)candidate(chain_, Address{}, chain_.tip_header().timestamp + SPACING, proposal);
        // Prepare and durably save before publishing or acknowledging admission.
        if (durable)
            persist_pool(proposal);
        pool_.swap(proposal);
        pool_ids_.swap(proposed_ids);
        pool_bytes_ += size;
    }
    void prune() {
        auto old = std::move(pool_);
        pool_.clear();
        pool_ids_.clear();
        pool_bytes_ = 0;
        for (const auto &t : old)
            try {
                admit(t, false);
            } catch (const Invalid &) {
            }
    }
    void reconcile(const Hash &old_tip) {
        prune();
        auto left = old_tip, right = blockid(chain_.tip_header());
        std::vector<Hash> detached;
        while (chain_.header(left).height > chain_.header(right).height) {
            detached.push_back(left);
            left = chain_.header(left).previous;
        }
        while (chain_.header(right).height > chain_.header(left).height)
            right = chain_.header(right).previous;
        while (left != right) {
            detached.push_back(left);
            left = chain_.header(left).previous;
            right = chain_.header(right).previous;
        }
        for (auto it = detached.rbegin(); it != detached.rend(); ++it) {
            auto body = chain_.block(*it);
            for (const auto &tx : body.transactions)
                if (tx.coinbase_height == 0)
                    try {
                        admit(tx, false);
                    } catch (const Invalid &) {
                    }
        }
    }
    void persist() {
        try {
            chain_.append(path_);
            persist_pool(pool_);
        } catch (const std::exception &e) {
            throw FatalStorage(std::string("durable storage failure; node stopped: ") + e.what());
        }
    }
    Bytes handle(char command, const Bytes &payload, bool authenticated = false) {
        if (!authenticated && (command == 'S' || command == 'B' || command == 'P'))
            need(read_budget_.take(), "read request rate limit; retry later");
        Reader r{payload};
        Bytes output;
        if (command == 'S') {
            r.end();
            auto current_tip = blockid(chain_.tip_header());
            if (!status_cached_ || status_tip_ != current_tip) {
            Bytes commitments;
            for (const auto &[op, c] : chain_.utxos()) {
                commitments.insert(commitments.end(), op.txid.begin(), op.txid.end());
                put32(commitments, op.index);
                for (unsigned j = 0; j < 8; j++)
                    commitments.push_back(static_cast<std::uint8_t>(c.output.value >> (8 * j)));
                commitments.insert(commitments.end(), c.output.owner.begin(), c.output.owner.end());
                for (unsigned j = 0; j < 8; j++)
                    commitments.push_back(static_cast<std::uint8_t>(c.height >> (8 * j)));
                commitments.push_back(c.coinbase ? 1 : 0);
            }
                status_utxo_hash_ = hash(commitments);
                status_tip_ = current_tip;
                status_cached_ = true;
            }
            auto text = std::string("network=") + NETWORK_NAME + " height=" + std::to_string(chain_.tip_header().height) +
                        " tip=" + hex(blockid(chain_.tip_header())) +
                        " auto_mining=" + (mining_owner_ ? std::string("on") : std::string("off")) +
                        " mempool=" + std::to_string(pool_.size()) +
                        " utxos=" + std::to_string(chain_.utxos().size()) +
                        " utxo_hash=" + hex(status_utxo_hash_) +
                        " last_hash_attempts=" + std::to_string(last_hashes_) +
                        " last_pipeline_hps=" + std::to_string(last_hps_) +
                        " mining_mode=" + mining_mode_ +
                        " mining_threads=" + std::to_string(mining_owner_ ? mining_threads_ : 0);
            return {text.begin(), text.end()};
        }
        if (command == 'B') {
            auto count = r.u32();
            need(count > 0 && count <= 65, "locator count");
            std::vector<Hash> locator;
            for (std::uint32_t i = 0; i < count; i++)
                locator.push_back(r.id());
            r.end();
            const auto &blocks = active_blocks();
            const auto &positions = active_positions_;
            std::size_t start = 0;
            bool found = false;
            for (const auto &id : locator) {
                auto it = positions.find(id);
                if (it != positions.end()) {
                    start = it->second + 1;
                    found = true;
                    break;
                }
            }
            need(found, "no common genesis");
            put32(output, 0);
            std::uint32_t returned = 0;
            for (std::size_t j = start; j < blocks.size() && returned < 32; j++) {
                auto bytes = encode(chain_.block(blocks[j]));
                if (output.size() + 4 + bytes.size() > PAYLOAD_LIMIT)
                    break;
                put32(output, static_cast<std::uint32_t>(bytes.size()));
                output.insert(output.end(), bytes.begin(), bytes.end());
                returned++;
            }
            for (unsigned j = 0; j < 4; j++)
                output[j] = static_cast<std::uint8_t>(returned >> (8 * j));
            return output;
        }
        if (command == 'P') {
            r.end();
            put32(output, static_cast<std::uint32_t>(pool_.size()));
            for (const auto &tx : pool_) {
                auto bytes = encode(tx);
                put32(output, static_cast<std::uint32_t>(bytes.size()));
                output.insert(output.end(), bytes.begin(), bytes.end());
            }
            return output;
        }
        if (command == 'T') {
            admit(decode_transaction(payload));
            return {};
        }
        if (command == 'M') {
            need(ACTIVE_NETWORK == Network::Regtest,
                 "mainnet uses continuous mining: start this node with --mine YOUR_ADDRESS");
            Address owner;
            auto a = r.take(32);
            std::copy(a.begin(), a.end(), owner.begin());
            auto count = r.u32();
            r.end();
            need(count > 0 && count <= 100, "mine count 1..100");
            auto started = std::chrono::steady_clock::now();
            last_hashes_ = 0;
            for (std::uint32_t j = 0; j < count; j++) {
                auto b = candidate(chain_, owner, chain_.tip_header().timestamp + SPACING, pool_);
                last_hashes_ += mine(b, 1000000);
                chain_.add(b);
                prune();
                persist();
            }
            auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - started)
                              .count();
            last_hps_ =
                micros > 0 ? last_hashes_ * 1000000 / static_cast<std::uint64_t>(micros) : 0;
            return handle('S', {}, true);
        }
        throw Invalid("unknown network command");
    }
    Bytes locator(const Hash &last) const {
        std::vector<Hash> ids;
        if (last != Hash{})
            ids.push_back(last);
        const auto &blocks = active_blocks();
        std::size_t cursor = blocks.size() - 1, step = 1;
        for (unsigned j = 0; j < 63; j++) {
            auto id = blocks[cursor];
            if (ids.empty() || ids.back() != id)
                ids.push_back(id);
            if (cursor == 0)
                break;
            cursor = cursor > step ? cursor - step : 0;
            if (j >= 10 && step < std::numeric_limits<std::size_t>::max() / 2)
                step *= 2;
        }
        auto gid = blocks.front();
        if (ids.back() != gid)
            ids.push_back(gid);
        Bytes payload;
        put32(payload, static_cast<std::uint32_t>(ids.size()));
        for (const auto &id : ids)
            payload.insert(payload.end(), id.begin(), id.end());
        return payload;
    }
    void import(const Bytes &blocks, const Bytes &transactions, Hash &last) {
        Reader r{blocks};
        auto count = r.u32();
        need(count <= 32, "block batch count");
        bool changed = false;
        auto previous_tip = blockid(chain_.tip_header());
        try {
            for (std::uint32_t j = 0; j < count; j++) {
                auto size = r.u32();
                need(size <= MAX_BLOCK_BYTES, "peer block size");
                auto b = decode_block(r.take(size));
                auto id = blockid(b.header);
                bool known = false;
                try {
                    (void)chain_.block(id);
                    known = true;
                } catch (const Invalid &) {
                }
                if (!known) {
                    need(ACTIVE_NETWORK != Network::Mainnet ||
                         b.header.timestamp <= control::unix_seconds() + 7200,
                         "block timestamp exceeds local future-time admission window");
                    chain_.add(b);
                    changed = true;
                }
                last = id;
            }
            r.end();
        } catch (...) {
            if (changed) {
                reconcile(previous_tip);
                persist();
            }
            throw;
        }
        if (changed) {
            reconcile(previous_tip);
            persist();
        }
        Reader p{transactions};
        count = p.u32();
        need(count <= 64, "peer mempool count");
        for (std::uint32_t j = 0; j < count; j++) {
            auto size = p.u32();
            need(size <= MAX_BLOCK_BYTES, "peer transaction size");
            auto tx = decode_transaction(p.take(size));
            try {
                admit(tx);
            } catch (const Invalid &) {
            }
        }
        p.end();
    }
};
Bytes read_tx(const std::string &path) {
    std::ifstream f(file_path(path), std::ios::binary | std::ios::ate);
    need(bool(f), "transaction open");
    auto size = f.tellg();
    need(size >= 0 && size <= MAX_BLOCK_BYTES, "transaction size");
    Bytes bytes(static_cast<std::size_t>(size));
    f.seekg(0);
    f.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    need(bool(f), "transaction read");
    return bytes;
}
} // namespace
int qbtc_entry(int argc, char **argv) {
    try {
#ifdef _WIN32
        WSADATA data{};
        need(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Winsock startup");
#endif
        need(crypto_available(), "ML-DSA-65 provider required");
        if (argc == 6 && std::string_view(argv[1]) == "benchmark" &&
            std::string_view(argv[2]) == "--seconds" && std::string_view(argv[4]) == "--mining-mode") {
            unsigned seconds = 0;
            std::string_view text = argv[3];
            auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), seconds);
            need(error == std::errc{} && end == text.data() + text.size(), "benchmark duration");
            auto mode = std::string_view(argv[5]);
            auto sample = calibrate_cpu(mode, seconds);
            std::cout << "{\"schema\":\"qbtc-cpu-calibration-v1\",\"network\":\"none\","
                         "\"benchmark_only\":true,\"mode\":\"" << mode << "\",\"threads\":"
                      << sample.threads << ",\"hashes\":" << sample.hashes
                      << ",\"elapsed_us\":" << sample.elapsed_us << "}\n";
            return 0;
        }
        if ((argc == 3 || (argc == 4 && std::string_view(argv[3]) == "--onion-adapter")) && std::string(argv[1]) == "status") {
            if (argc == 4) {
                need(std::string_view(argv[2]) == "127.0.0.1:19022" || std::string_view(argv[2]) == "127.0.0.1:19024" || std::string_view(argv[2]) == "127.0.0.1:19026" || std::string_view(argv[2]) == "127.0.0.1:19028", "onion adapter must be a dedicated loopback synchronization lane");
                enable_onion_adapter();
            }
            auto bytes = request(endpoint(argv[2]), 'S', {});
            std::cout << std::string(bytes.begin(), bytes.end()) << "\n";
            return 0;
        }
        if (argc == 5 && std::string_view(argv[1]) == "local-status" &&
            std::string_view(argv[3]) == "--control-file") {
            control::Authenticator auth(argv[4], false);
            auto payload = auth.seal('S', {}, control::unix_seconds());
            auto response = request(port(argv[2]), 'S', payload);
            std::cout << std::string(response.begin(), response.end()) << "\n";
            return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "submit") {
            request(endpoint(argv[2]), 'T', read_tx(argv[3]));
            std::cout << "Transaction admitted\n";
            return 0;
        }
        if (argc == 7 && std::string(argv[1]) == "mine") {
            auto owner = wallet::parse_address(argv[3]);
            Bytes payload(owner.begin(), owner.end());
            unsigned n = 0;
            std::string_view s = argv[4];
            auto [p, e] = std::from_chars(s.data(), s.data() + s.size(), n);
            need(e == std::errc{} && p == s.data() + s.size() && n > 0 && n <= 100,
                 "mine count 1..100");
            put32(payload, n);
            need(std::string_view(argv[5]) == "--control-file", "--control-file required");
            control::Authenticator auth(argv[6], false);
            auto authenticated = auth.seal('M', payload, control::unix_seconds());
            auto response = request(port(argv[2]), 'M', authenticated);
            std::cout << std::string(response.begin(), response.end()) << "\n";
            return 0;
        }
        if (argc >= 6 && argc <= 28 && std::string(argv[1]) == "serve") {
            auto own = port(argv[3]);
            need(std::string_view(argv[4]) == "--control-port", "--control-port required");
            auto control_port = port(argv[5]);
            need(own != control_port, "P2P and control ports must differ");
            bool internet_test = false;
            for (int j = 6; j < argc; ++j)
                if (std::string_view(argv[j]) == PUBLIC_NETWORK_FLAG)
                    internet_test = true;
            bool discover = false;
            bool onion_adapter = false;
            std::optional<Endpoint> advertised_endpoint;
            std::string bind_address = "127.0.0.1";
            std::string mining_mode = "medium", mining_mode_file;
            std::optional<Address> mine_owner;
            std::optional<std::uint64_t> mine_count;
            std::vector<Endpoint> configured;
            for (int j = 6; j < argc; j++) {
                std::string_view argument = argv[j];
                if (argument == PUBLIC_NETWORK_FLAG) {
                    continue;
                } else if (argument == "--onion-adapter") {
                    need(!onion_adapter, "duplicate --onion-adapter");
                    onion_adapter = true;
                } else if (argument == "--discover") {
                    need(!discover, "duplicate --discover");
                    discover = true;
                } else if (argument == "--advertise") {
                    need(j + 1 < argc && !advertised_endpoint, "--advertise requires one IPv4:port");
                    advertised_endpoint = endpoint(argv[++j], internet_test);
                } else if (argument == "--bind") {
                    need(j + 1 < argc, "--bind requires private IPv4");
                    bind_address = private_address(argv[++j]);
                } else if (argument == "--mining-mode") {
                    need(j + 1 < argc, "--mining-mode requires low, medium or high");
                    mining_mode = argv[++j];
                    (void)mining_threads(mining_mode);
                } else if (argument == "--mining-mode-file") {
                    need(j + 1 < argc && mining_mode_file.empty(), "--mining-mode-file requires one path");
                    mining_mode_file = argv[++j];
                } else if (argument == "--mine") {
                    need(j + 1 < argc && !mine_owner, "--mine requires one wallet address");
                    mine_owner = wallet::parse_address(argv[++j]);
                } else if (argument == "--mine-count") {
                    need(j + 1 < argc && !mine_count, "--mine-count requires one count");
                    std::uint64_t n = 0;
                    std::string_view value = argv[++j];
                    auto [end, error] =
                        std::from_chars(value.data(), value.data() + value.size(), n);
                    need(error == std::errc{} && end == value.data() + value.size() && n > 0 &&
                             n <= 250000,
                         "automatic mine count must be 1..250000");
                    mine_count = n;
                } else {
                    need(configured.size() < 8, "at most eight configured peers");
                    configured.push_back(endpoint(argument, internet_test));
                }
            }
            need(!internet_test || discover || !configured.empty(), "--internet-test requires explicit peers");
            need(!mine_count || mine_owner, "--mine-count requires --mine");
            if (onion_adapter) {
                need(!discover && !internet_test && !configured.empty() && configured.size() <= 4 &&
                     std::all_of(configured.begin(), configured.end(), [](const Endpoint &e) {
                         return e.host == "127.0.0.1" && (e.number == 19022 || e.number == 19024 || e.number == 19026 || e.number == 19028);
                     }),
                     "--onion-adapter requires one to four dedicated loopback lanes and discovery off");
                enable_onion_adapter();
            }
            for (const auto &remote : configured)
                need(remote != Endpoint{bind_address, own, internet_test} &&
                         remote != Endpoint{"127.0.0.1", control_port, internet_test},
                     "peer is a local endpoint");
            PeerScope scope = bind_address.starts_with("127.") ? PeerScope::Loopback :
                              (internet_test ? PeerScope::Public : PeerScope::Lan);
            need(!advertised_endpoint || discover, "--advertise requires --discover");
            need(!discover || scope != PeerScope::Public || advertised_endpoint,
                 "Internet discovery requires --advertise PUBLIC_IPV4:P2P_PORT");
            Endpoint self = advertised_endpoint.value_or(Endpoint{bind_address, own, internet_test});
            if (discover) {
                need(peer_in_scope(self, scope), "advertised address outside discovery scope");
                need(self.number != control_port, "never advertise the control port");
                for (const auto &remote : configured)
                    need(peer_in_scope(remote, scope), "configured peer outside discovery scope");
            }
            storage::RunLock lock(argv[2]);
            std::optional<PeerBook> book;
            if (discover) {
                std::vector<Endpoint> blocked{self, {bind_address, own}, {bind_address, control_port},
                                              {"127.0.0.1", own}, {"127.0.0.1", control_port},
                                              {self.host, control_port}};
                auto suffix = scope == PeerScope::Public ? ".peers-public" :
                              (scope == PeerScope::Lan ? ".peers-lan" : ".peers-loopback");
                book.emplace(std::string(argv[2]) + suffix, scope, std::move(blocked));
                for (const auto &remote : configured) book->add(remote, true);
            }
            control::Authenticator auth(std::string(argv[2]) + ".control", true);
            Node node(argv[2]);
            node.set_mining_mode(mining_mode);
            if (mine_owner)
                node.enable_mining(*mine_owner, mine_count);
            struct Peer {
                Hash last{};
                std::future<SyncReply> pending;
                std::chrono::steady_clock::time_point retry{};
                unsigned failures = 0;
            };
            std::map<Endpoint, Peer> peers;
            for (const auto &remote : book ? book->known() : configured)
                peers.try_emplace(remote);
            auto monotonic_ms = [] {
                return std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            };
            Connection listener(::socket(AF_INET, SOCK_STREAM, 0));
            Connection control_listener(::socket(AF_INET, SOCK_STREAM, 0));
            auto listen_on = [](Connection &connection, std::uint16_t listen_port,
                                const std::string &host) {
                need(connection.s != BAD, "listen socket");
                int enabled = 1;
#ifdef _WIN32
                need(setsockopt(connection.s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                                reinterpret_cast<const char *>(&enabled), sizeof(enabled)) == 0,
                     "exclusive listener socket");
#else
                need(connection.s < FD_SETSIZE, "listen descriptor limit");
                need(setsockopt(connection.s, SOL_SOCKET, SO_REUSEADDR,
                                reinterpret_cast<const char *>(&enabled), sizeof(enabled)) == 0,
                     "reuse socket");
#endif
                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_port = htons(listen_port);
                need(inet_pton(AF_INET, host.c_str(), &address.sin_addr) == 1, "listen IPv4");
                need(::bind(connection.s, reinterpret_cast<sockaddr *>(&address),
                            sizeof(address)) == 0,
                     "listen bind");
                need(::listen(connection.s, 16) == 0, "listen backlog");
            };
            listen_on(listener, own, bind_address);
            listen_on(control_listener, control_port, "127.0.0.1");
            std::cout << "QBTC " << VERSION << " " << NETWORK_NAME << " P2P " << bind_address << ":" << own
                      << " control 127.0.0.1:" << control_port
                      << (internet_test ? " public transport" : " private interface transport")
                      << (discover ? " discovery on" : " discovery off") << std::endl;
            struct Incoming {
                // Futures are destroyed before the connection, including during shutdown.
                std::unique_ptr<Connection> connection;
                bool controlling = false;
                std::uint32_t source_ip = 0;
                std::future<Frame> received;
                std::future<void> sent;
            };
            std::vector<std::unique_ptr<Incoming>> clients;
            auto next = std::chrono::steady_clock::now();
            auto next_mode_check = std::chrono::steady_clock::now();
            for (;;) {
                if (!mining_mode_file.empty() && std::chrono::steady_clock::now() >= next_mode_check) {
                    // Local panel preference only. Malformed/partial writes keep the current mode.
                    std::ifstream preference(file_path(mining_mode_file), std::ios::binary);
                    char buffer[17]{};
                    preference.read(buffer, 17);
                    auto size = preference.gcount();
                    if (size > 0 && size < 17) {
                        std::string value(buffer, static_cast<std::size_t>(size));
                        while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
                        if (value == "low" || value == "medium" || value == "high") node.set_mining_mode(value);
                    }
                    next_mode_check = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                }
                fd_set read;
                FD_ZERO(&read);
                FD_SET(listener.s, &read);
                FD_SET(control_listener.s, &read);
                timeval delay{0, 100000};
                auto ready =
                    ::select(static_cast<int>(std::max(listener.s, control_listener.s) + 1), &read,
                             nullptr, nullptr, &delay);
                need(ready >= 0, "socket select");
                for (const auto endpoint : {control_listener.s, listener.s}) {
                    if (ready <= 0 || !FD_ISSET(endpoint, &read))
                        continue;
                    bool controlling = endpoint == control_listener.s;
                    sockaddr_in source{};
#ifdef _WIN32
                    int source_size = sizeof(source);
#else
                    socklen_t source_size = sizeof(source);
#endif
                    auto accepted = std::make_unique<Connection>(::accept(
                        endpoint, reinterpret_cast<sockaddr *>(&source), &source_size));
                    if (accepted->s == BAD)
                        continue;
                    const auto source_ip = ntohl(source.sin_addr.s_addr);
                    bool source_allowed = incoming_source_allowed(source_ip, internet_test, configured);
                    if (discover && internet_test) {
                        in_addr address{};
                        address.s_addr = htonl(source_ip);
                        char text[INET_ADDRSTRLEN]{};
                        need(inet_ntop(AF_INET, &address, text, sizeof(text)) != nullptr, "source IPv4 text");
                        source_allowed = (source_ip >> 24) == 127 || std::string_view(text) == bind_address ||
                                         peer_in_scope(Endpoint{text, own, true}, scope);
                    }
                    if (!controlling && !source_allowed) continue;
#ifndef _WIN32
                    if (accepted->s >= FD_SETSIZE)
                        continue;
#endif
                    auto busy = [](const auto &client) {
                        return !client->sent.valid() || client->sent.wait_for(std::chrono::milliseconds(0)) !=
                               std::future_status::ready;
                    };
                    auto occupied = std::count_if(clients.begin(), clients.end(),
                                                  [controlling, &busy](const auto &client) {
                                                      return client->controlling == controlling && busy(client);
                                                  });
                    if (occupied >= (controlling ? 2 : 8))
                        continue;
                    if (discover && !controlling) {
                        auto same_source = std::count_if(clients.begin(), clients.end(), [&](const auto &c) {
                            return !c->controlling && c->source_ip == source_ip && busy(c);
                        });
                        auto same_group = std::count_if(clients.begin(), clients.end(), [&](const auto &c) {
                            return !c->controlling && (c->source_ip >> 8) == (source_ip >> 8) && busy(c);
                        });
                        if (same_source >= 2 || same_group >= 4) continue;
                    }
                    auto client = std::make_unique<Incoming>();
                    client->source_ip = source_ip;
                    client->controlling = controlling;
                    auto socket = accepted->s;
                    client->connection = std::move(accepted);
                    client->received = std::async(std::launch::async, [socket, controlling] {
                        timeout(socket);
                        return receive_frame(socket, controlling ? "MS" : "SBPTAD");
                    });
                    clients.push_back(std::move(client));
                }
                // Workers perform only bounded socket I/O; consensus stays on this thread.
                for (auto it = clients.begin(); it != clients.end();) {
                    auto &client = **it;
                    if (client.sent.valid()) {
                        if (client.sent.wait_for(std::chrono::milliseconds(0)) ==
                            std::future_status::ready) {
                            try {
                                client.sent.get();
                            } catch (const std::exception &) {
                            }
                            it = clients.erase(it);
                        } else
                            ++it;
                        continue;
                    }
                    if (client.received.wait_for(std::chrono::milliseconds(0)) !=
                        std::future_status::ready) {
                        ++it;
                        continue;
                    }
                    Frame reply;
                    try {
                        auto [command, payload] = client.received.get();
                        reply.command = command;
                        Bytes response;
                        if (command == 'D') {
                            need(discover, "peer discovery disabled");
                            if (!payload.empty()) {
                                auto announced = decode_peers(payload);
                                need(announced.size() == 1, "one endpoint announcement required");
                                in_addr parsed{};
                                need(inet_pton(AF_INET, announced[0].host.c_str(), &parsed) == 1,
                                     "announcement IPv4");
                                // A remote cannot register an arbitrary host as its own endpoint.
                                if (ntohl(parsed.s_addr) == client.source_ip) book->add(announced[0]);
                            }
                            auto shared = book->verified();
                            // Rotate so a large book does not permanently hide later entries.
                            static std::size_t rotation = 0;
                            if (!shared.empty()) {
                                std::rotate(shared.begin(), shared.begin() +
                                    static_cast<std::ptrdiff_t>(rotation++ % shared.size()), shared.end());
                            }
                            if (shared.size() >= MAX_SHARED_PEERS) shared.resize(MAX_SHARED_PEERS - 1);
                            shared.erase(std::remove_if(shared.begin(), shared.end(), [&](const auto &p) {
                                return p.host == self.host && p.number == self.number;
                            }), shared.end());
                            shared.insert(shared.begin(), self);
                            response = encode_peers(shared);
                        } else if (command == 'A') {
                            if (onion_adapter || discover || internet_test || bind_address != "127.0.0.1") {
                                put32(response, 0);
                            } else {
                                if (!payload.empty()) {
                                    Reader announced{payload};
                                    auto remote = announced.u32();
                                    announced.end();
                                    need(remote >= 1024 && remote <= 65535,
                                         "announced peer port range");
                                    if (remote != own && remote != control_port && peers.size() < 8)
                                        peers.try_emplace(Endpoint{
                                            "127.0.0.1", static_cast<std::uint16_t>(remote)});
                                }
                                put32(response,
                                      static_cast<std::uint32_t>(std::count_if(
                                          peers.begin(), peers.end(), [](const auto &item) {
                                              return item.first.host == "127.0.0.1";
                                          })));
                                for (const auto &[p, unused] : peers) {
                                    (void)unused;
                                    if (p.host == "127.0.0.1")
                                        put32(response, p.number);
                                }
                            }
                        } else if (client.controlling) {
                            auto body = auth.open(command, payload, control::unix_seconds());
                            response = node.handle(command, body, true);
                        } else {
                            response = node.handle(command, payload);
                            if (command == 'S') {
                                std::string info = " peer_known=" + std::to_string(peers.size()) +
                                    " peer_verified=" + std::to_string(book ? book->verified().size() : 0) +
                                    " peer_discovery=" + (discover ? std::string("on") : std::string("off"));
                                response.insert(response.end(), info.begin(), info.end());
                            }
                        }
                        reply.payload = std::move(response);
                    } catch (const PeerStorageFailure &) {
                        throw;
                    } catch (const FatalStorage &) {
                        throw;
                    } catch (const std::exception &e) {
                        std::string reason = e.what();
                        reason.resize(std::min<std::size_t>(reason.size(), 256));
                        reply.command = 'E';
                        reply.payload = Bytes(reason.begin(), reason.end());
                    }
                    auto socket = client.connection->s;
                    client.sent =
                        std::async(std::launch::async, [socket, reply = std::move(reply)] {
                            timeout(socket);
                            send_frame(socket, reply.command, reply.payload);
                        });
                    ++it;
                }
                for (auto &[p, peer] : peers) {
                    (void)p;
                    if (peer.pending.valid() && peer.pending.wait_for(std::chrono::milliseconds(
                                                    0)) == std::future_status::ready) {
                        try {
                            auto reply = peer.pending.get();
                            node.import(reply.blocks, reply.transactions, peer.last);
                            if (book) {
                                if (!reply.discovered.empty()) {
                                    auto learned = decode_peers(reply.discovered); // validate whole list first
                                    for (const auto &entry : learned) book->add(entry);
                                }
                            }
                            peer.failures = 0;
                            peer.retry = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                            Reader addresses{reply.advertised};
                            auto count = addresses.u32();
                            need(count <= 8, "advertised peer limit");
                            for (std::uint32_t j = 0; j < count; j++) {
                                auto discovered = addresses.u32();
                                need(discovered >= 1024 && discovered <= 65535,
                                     "advertised port range");
                                if (!onion_adapter && !discover && !internet_test && bind_address == "127.0.0.1" && p.host == bind_address &&
                                    discovered != own && discovered != control_port &&
                                    peers.size() < 8)
                                    peers.try_emplace(Endpoint{
                                        "127.0.0.1", static_cast<std::uint16_t>(discovered)});
                            }
                            addresses.end();
                            if (book) book->good(p, monotonic_ms());
                        } catch (const PeerStorageFailure &) {
                        throw;
                    } catch (const FatalStorage &) {
                            throw;
                        } catch (const std::exception &) {
                            if (book) book->bad(p, monotonic_ms());
                            peer.failures = std::min(peer.failures + 1, 9u);
                            peer.retry = std::chrono::steady_clock::now() + std::chrono::milliseconds(
                                std::min<std::int64_t>(300000, std::int64_t(1000) << peer.failures));
                        }
                    }
                }
                if (book) {
                    auto known = book->known();
                    for (const auto &remote : known) peers.try_emplace(remote);
                    std::set<Endpoint> retained(known.begin(), known.end());
                    for (auto it = peers.begin(); it != peers.end();) {
                        if (!it->second.pending.valid() && !retained.contains(it->first)) it = peers.erase(it);
                        else ++it;
                    }
                }
                node.mine_step();
                if (std::chrono::steady_clock::now() >= next) {
                    std::size_t active_requests = 0;
                    for (const auto &[p, peer] : peers) { (void)p; if (peer.pending.valid()) ++active_requests; }
                    std::vector<Endpoint> candidates;
                    if (book) candidates = book->due(monotonic_ms(), MAX_OUTBOUND_PEERS - active_requests);
                    else for (const auto &[p, peer] : peers)
                        if (!peer.pending.valid() && peer.retry <= std::chrono::steady_clock::now())
                            candidates.push_back(p);
                    for (const auto &p : candidates) {
                        auto &peer = peers.at(p);
                        need(!peer.pending.valid(), "peer scheduler duplicate in-flight request");
                        auto locator = node.locator(peer.last);
                        peer.pending = std::async(std::launch::async,
                            [p, own, bind_address, internet_test, discover, onion_adapter, self,
                             locator = std::move(locator)] {
                                return fetch_sync(p, locator,
                                    !onion_adapter && !discover && !internet_test && p.host == bind_address &&
                                    bind_address == "127.0.0.1" ? own : 0,
                                    discover, discover ? std::optional<Endpoint>(self) : std::nullopt);
                            });
                    }
                    next = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                }
            }
        }
        std::cerr << "Usage: qbtc-peer serve CHAIN P2P_PORT --control-port CONTROL_PORT "
                     "[--bind PRIVATE_IPV4] [" << PUBLIC_NETWORK_FLAG << "] [--discover] "
                     "[--advertise IPV4:P2P_PORT] [--onion-adapter] [--mine ADDRESS] [--mine-count N] [--mining-mode low|medium|high] [--mining-mode-file PATH] [PEER_IP:PORT ...]\n"
                  << "qbtc-peer status IP:PORT [--onion-adapter] | submit IP:PORT TXFILE\n"
                  << "Network profile: " << NETWORK_NAME << ". Mining control is loopback-only.\n";
        return 2;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}

QBTC_ENTRY(qbtc_entry)
