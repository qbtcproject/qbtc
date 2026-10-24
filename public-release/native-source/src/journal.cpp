#include "qbtc/platform.hpp"
#include "qbtc/consensus.hpp"
#include "qbtc/storage.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace qbtc {
namespace {
constexpr std::uint64_t FILE_LIMIT = 1024ULL * 1024 * 1024;
constexpr std::uint32_t FRAME_LIMIT = 64 * 1024 * 1024;
constexpr std::array<std::uint8_t, 8> COMMIT = {'Q', 'C', 'O', 'M', 'M', 'I', 'T', 1};
void require(bool ok, const char *text) {
    if (!ok)
        throw Invalid(text);
}
void put32(Bytes &out, std::uint32_t n) {
    for (unsigned j = 0; j < 4; j++)
        out.push_back(static_cast<std::uint8_t>(n >> (8 * j)));
}
void raw(Bytes &out, std::span<const std::uint8_t> data) {
    require(data.size() <= FRAME_LIMIT - out.size(), "journal frame limit");
    out.insert(out.end(), data.begin(), data.end());
}
struct Reader {
    std::span<const std::uint8_t> data;
    std::size_t pos = 0;
    std::span<const std::uint8_t> take(std::size_t n) {
        require(n <= data.size() - pos, "truncated journal payload");
        auto part = data.subspan(pos, n);
        pos += n;
        return part;
    }
    std::uint32_t u32() {
        auto b = take(4);
        std::uint32_t n = 0;
        for (unsigned j = 0; j < 4; j++)
            n |= std::uint32_t(b[j]) << (8 * j);
        return n;
    }
    Hash id() {
        Hash h{};
        auto b = take(64);
        std::copy(b.begin(), b.end(), h.begin());
        return h;
    }
};
Bytes journal_header(Network network) {
    Bytes b = {'Q', 'B', 'J', 'L', 1};
    put32(b, static_cast<std::uint32_t>(network));
    auto id = blockid(genesis(network).header);
    raw(b, id);
    return b;
}
void read_exact(std::ifstream &file, std::span<std::uint8_t> bytes) {
    file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(bool(file), "journal read failure");
}
} // namespace
std::uint64_t Chain::recovered_tail_bytes() const {
    return journal_ ? journal_->observed_bytes - journal_->committed_bytes : 0;
}
void Chain::append(const std::string &path) {
    bool continuing = journal_ && journal_->path == path;
    require(continuing || !std::filesystem::exists(file_path(path)) || snapshot_origin_ == path,
            "refusing to replace an unrelated existing journal");
    auto first = continuing ? journal_->blocks : 0;
    require(first <= arrival_.size(), "journal cursor blocks");
    auto previous = continuing ? journal_->digest : hash(journal_header(network_));
    if (continuing && first == arrival_.size() && journal_->persisted_tip == tip_ &&
        recovered_tail_bytes() == 0)
        return;
    Bytes payload;
    raw(payload, previous);
    raw(payload, tip_);
    require(arrival_.size() - first <= 250000, "journal batch block count");
    put32(payload, static_cast<std::uint32_t>(arrival_.size() - first));
    struct Location {
        Hash id;
        std::uint64_t offset;
        std::uint32_t length;
        Hash digest;
    };
    std::vector<Location> locations;
    locations.reserve(arrival_.size() - first);
    auto body_path = path;
    auto frame_offset = continuing ? journal_->committed_bytes : journal_header(network_).size();
    for (std::size_t j = first; j < arrival_.size(); j++) {
        auto bytes = encode(block(arrival_[j]));
        require(payload.size() + 4 + bytes.size() <= FRAME_LIMIT, "journal frame limit");
        put32(payload, static_cast<std::uint32_t>(bytes.size()));
        locations.push_back({arrival_[j], frame_offset + 4 + payload.size(),
                             static_cast<std::uint32_t>(bytes.size()), hash(bytes)});
        raw(payload, bytes);
    }
    auto digest = hash(payload);
    Bytes frame;
    put32(frame, static_cast<std::uint32_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.insert(frame.end(), digest.begin(), digest.end());
    frame.insert(frame.end(), COMMIT.begin(), COMMIT.end());
    auto offset = continuing ? journal_->committed_bytes : journal_header(network_).size();
    require(offset + frame.size() <= FILE_LIMIT, "journal file limit");
    // Preallocate metadata before a durable write. Nothing allocating is needed to
    // advance the cursor after fsync succeeds.
    JournalCursor next{
        path, digest, tip_, arrival_.size(), offset + frame.size(), offset + frame.size()};
    if (continuing)
        storage::append_durable(path, frame, journal_->observed_bytes, journal_->committed_bytes);
    else {
        auto bytes = journal_header(network_);
        bytes.insert(bytes.end(), frame.begin(), frame.end());
        storage::replace_atomic(path, bytes);
    }
    for (const auto &location : locations) {
        auto &entry = entries_.at(location.id);
        entry.offset = location.offset;
        entry.length = location.length;
        entry.body_digest = location.digest;
        if (entry.body) {
            block_bytes_ -= location.length;
            entry.body.reset();
        }
    }
    body_path_.swap(body_path);
    journal_ = std::move(next);
    snapshot_origin_.clear();
}
Chain Chain::load_journal(Network network, const std::string &path) {
    std::ifstream file(file_path(path), std::ios::binary | std::ios::ate);
    require(bool(file), "journal open");
    auto size = file.tellg();
    require(size >= 73 && static_cast<std::uint64_t>(size) <= FILE_LIMIT, "journal file limit");
    auto observed = static_cast<std::uint64_t>(size);
    file.seekg(0);
    Bytes prefix(73);
    read_exact(file, prefix);
    require(prefix == journal_header(network), "journal network/genesis header");
    auto digest = hash(prefix);
    std::uint64_t offset = 73;
    Chain chain(network);
    chain.body_path_ = path;
    while (offset < observed) {
        auto remaining = observed - offset;
        if (remaining < 4)
            break;
        std::array<std::uint8_t, 4> length{};
        read_exact(file, length);
        Reader count_reader{length};
        auto n = count_reader.u32();
        require(n >= 132 && n <= FRAME_LIMIT, "journal frame length");
        const auto frame_size = std::uint64_t(n) + 4 + 64 + 8;
        if (frame_size > remaining)
            break;
        Bytes payload(n);
        read_exact(file, payload);
        Hash checksum{};
        read_exact(file, checksum);
        std::array<std::uint8_t, 8> commit{};
        read_exact(file, commit);
        require(commit == COMMIT, "journal commit marker");
        require(hash(payload) == checksum, "journal committed checksum");
        Reader r{payload};
        require(r.id() == digest, "journal digest chain");
        auto saved_tip = r.id();
        auto count = r.u32();
        require(count <= 250000, "journal block count");
        for (std::uint32_t j = 0; j < count; j++) {
            auto length_bytes = r.u32();
            require(length_bytes <= MAX_BLOCK_BYTES, "journal block bytes");
            auto body_offset = offset + 4 + r.pos;
            auto raw_block = r.take(length_bytes);
            auto id = chain.add(decode_block(raw_block));
            auto &entry = chain.entries_.at(id);
            entry.offset = body_offset;
            entry.length = length_bytes;
            entry.body_digest = hash(raw_block);
            entry.body.reset();
            chain.block_bytes_ -= length_bytes;
        }
        require(r.pos == payload.size(), "journal trailing payload");
        chain.restore_tip(saved_tip);
        digest = checksum;
        offset += frame_size;
    }
    chain.journal_ =
        JournalCursor{path, digest, chain.tip_, chain.arrival_.size(), offset, observed};
    return chain;
}
} // namespace qbtc
