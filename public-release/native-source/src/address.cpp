#include "qbtc/address.hpp"
namespace qbtc::wallet {
namespace {
void need(bool ok, const char *msg) {
    if (!ok)
        throw Invalid(msg);
}
constexpr char ALPHABET[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
std::uint32_t polymod(const Bytes &values) {
    constexpr std::uint32_t gen[] = {0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3};
    std::uint32_t chk = 1;
    for (auto v : values) {
        auto top = chk >> 25;
        chk = ((chk & 0x1ffffff) << 5) ^ v;
        for (unsigned j = 0; j < 5; j++)
            if ((top >> j) & 1)
                chk ^= gen[j];
    }
    return chk;
}
Bytes expand(std::string_view hrp) {
    Bytes b;
    for (char c : hrp)
        b.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(c) >> 5));
    b.push_back(0);
    for (char c : hrp)
        b.push_back(static_cast<std::uint8_t>(c & 31));
    return b;
}
std::string prefix(Network n) {
    if (n == Network::Regtest) return "qbr";
#ifdef QBTC_MAINNET
    if (n == Network::Mainnet) return "qbc";
    throw Invalid("address network is not enabled in this mainnet build");
#else
    throw Invalid("wallet supports regtest only until network activation");
#endif
}
} // namespace
std::string address(const Address &owner, Network network) {
    auto hrp = prefix(network);
    Bytes data{1}; // v1 address suite, not a Bitcoin witness address
    std::uint32_t acc = 0;
    unsigned bits = 0;
    for (auto byte : owner) {
        acc = ((acc << 8) | byte) & 0xffff;
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            data.push_back(static_cast<std::uint8_t>((acc >> bits) & 31));
        }
    }
    if (bits)
        data.push_back(static_cast<std::uint8_t>((acc << (5 - bits)) & 31));
    auto values = expand(hrp);
    values.insert(values.end(), data.begin(), data.end());
    values.insert(values.end(), 6, 0);
    auto checksum = polymod(values) ^ 0x2bc830a3;
    std::string result = hrp + "1";
    for (auto d : data)
        result += ALPHABET[d];
    for (unsigned j = 0; j < 6; j++)
        result += ALPHABET[(checksum >> (5 * (5 - j))) & 31];
    return result;
}
Address parse_address(std::string_view text, Network network) {
    auto hrp = prefix(network);
    need(text.size() == 63 && text.substr(0, 4) == hrp + "1", "address network or length");
    Bytes data;
    for (std::size_t i = 4; i < text.size(); i++) {
        auto found = std::string_view(ALPHABET).find(text[i]);
        need(found != std::string_view::npos, "address character or case");
        data.push_back(static_cast<std::uint8_t>(found));
    }
    auto values = expand(hrp);
    values.insert(values.end(), data.begin(), data.end());
    need(polymod(values) == 0x2bc830a3, "address checksum");
    need(data[0] == 1, "address version");
    Address result{};
    std::uint32_t acc = 0;
    unsigned bits = 0;
    std::size_t count = 0;
    for (std::size_t i = 1; i < data.size() - 6; i++) {
        acc = ((acc << 5) | data[i]) & 0xffff;
        bits += 5;
        while (bits >= 8) {
            bits -= 8;
            need(count < result.size(), "address overflow");
            result[count++] = static_cast<std::uint8_t>((acc >> bits) & 255);
        }
    }
    need(count == 32 && bits == 4 && ((acc & 15) == 0), "address padding");
    need(address(result, network) == text, "noncanonical address");
    return result;
}
} // namespace qbtc::wallet
