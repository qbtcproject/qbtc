#include "qbtc/address.hpp"
#include "qbtc/wallet.hpp"
#include "qbtc/control.hpp"
#include "qbtc/mempool.hpp"
#include "qbtc/peers.hpp"
#include "qbtc/storage.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
using namespace qbtc;
namespace {
unsigned checks=0;
void check(bool ok, const char *message) { ++checks; if(!ok) throw std::runtime_error(message); }
template<class F> void rejects(F f) {
 bool bad=false;try {f();} catch(const Invalid &) {bad=true;}
 check(bad,"expected network rejection");
}
}
int main() {
#ifdef _WIN32
 WSADATA data{};check(WSAStartup(MAKEWORD(2,2),&data)==0,"Winsock");
#endif
 check(ACTIVE_NETWORK==Network::Mainnet,"mainnet build profile");
 auto g=genesis(Network::Mainnet);
 check(hex(blockid(g.header))=="000006d71412362513fb97c48c0a1371b0edd3e7e6a29e91a8318f889181e5dd1255621ebcc717083a1a135c57c12b6c3885e2e0dbfac410d43089fbb49ed73c","frozen genesis");
 check(g.transactions.empty() && subsidy(0)==0,"genesis allocates no funds");
 check(g.header.height==0 && g.header.previous==Hash{} && valid_pow(g.header),"genesis proof and origin");
 check(hex(blockid(genesis(Network::Regtest).header))=="1b2161970c685bebb89fc0046a2a63582dce2bf417c8653038151e1db76141c87cce922cc340c8e7cb4f38c21119e39ea319a69ccb54fb3c2e284256b093cce5","regtest genesis unchanged");
 rejects([] {Chain reserved(Network::Testnet);});
 Chain main(Network::Mainnet),test(Network::Regtest);
 check(main.utxos().empty(),"no initial balances");
 check(hex(main.expected_target(blockid(g.header)))=="0000000148aafe12826323f74d7530852be32f6451d74282a550540100748422f5eeaef2a8d5f063f78d35783e71af206f734d4e7001331692e1ad82d828049c","calibrated target enforced at height one");
 rejects([] { Chain old(static_cast<Network>(0x51424d31)); });
 check(main.tip_header().network==Network::Mainnet,"mainnet chain identity");
 check(total_emission()<=MAX_MONEY && total_emission()==2099994997690000ULL,"fixed monetary upper bound");
 auto dir=std::filesystem::temp_directory_path()/("qbtc-mainnet-isolation-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 std::filesystem::create_directory(dir);
 try {
  auto main_path=(dir/"main.db").string(),test_path=(dir/"regtest.db").string();
  main.append(main_path);test.append(test_path);
  auto before=storage::read_bounded(test_path,100000);
  rejects([&] {Chain::load(Network::Mainnet,test_path);});
  check(storage::read_bounded(test_path,100000)==before,"wrong-network chain not modified");
  rejects([&] {Chain::load(Network::Regtest,main_path);});
  check(Chain::load(Network::Mainnet,main_path).utxos().empty(),"mainnet height-zero restart");
  auto key=wallet::Key::generate();auto address=wallet::address(key.owner());
  check(address.starts_with("qbc1") && wallet::parse_address(address)==key.owner(),"mainnet address roundtrip");
  auto test_address=wallet::address(key.owner(),Network::Regtest);
  rejects([&] {wallet::parse_address(test_address);});
  rejects([&] {wallet::parse_address(address,Network::Regtest);});
  auto path=(dir/"wallet.qwlt").string();
  key.save(path,"Mainnet-fixture-password-28!");
  auto file=storage::read_bounded(path,85);
  check(file[5]==0x32 && file[6]==0x4d && file[7]==0x42 && file[8]==0x51,"wallet mainnet network tag");
  check(wallet::Key::load(path,"Mainnet-fixture-password-28!").owner()==key.owner(),"encrypted mainnet wallet recovery");
  auto changed=file;changed[6]=0x52;auto wrong=(dir/"wrong-wallet").string();storage::write_new(wrong,changed);
  rejects([&] {wallet::Key::load(wrong,"Mainnet-fixture-password-28!");});
  auto main_pool=mempool::encode_file(Network::Mainnet,{});
  rejects([&] {mempool::decode_file(Network::Regtest,main_pool);});
  auto reg_pool=mempool::encode_file(Network::Regtest,{});
  rejects([&] {mempool::decode_file(Network::Mainnet,reg_pool);});
  auto peers=(dir/"peers").string();net::PeerBook book(peers,net::PeerScope::Loopback,{});
  net::Endpoint remote{"127.0.0.1",19021};book.add(remote);book.good(remote,0);
  auto peer_file=storage::read_bounded(peers,1024);
  check(peer_file[5]==0x32 && peer_file[6]==0x4d,"peer-store network tag");
  check(net::PeerBook(peers,net::PeerScope::Loopback,{}).known().size()==1,"mainnet peer-store restart");
  Transaction tx;tx.inputs.push_back({Outpoint{Hash{},0},key.public_key(),{}});tx.outputs.push_back({1,key.owner()});
  Output previous{2,key.owner()};
  auto msg=signature_message(Network::Mainnet,tx,0,previous);auto sig=key.sign(msg);
  check(verify_signature(key.public_key(),msg,sig),"mainnet signature domain verifies");
  check(!verify_signature(key.public_key(),signature_message(Network::Regtest,tx,0,previous),sig),"regtest signature replay rejected");
  std::filesystem::remove_all(dir);
  std::cout<<"PASS mainnet identity/isolation "<<checks<<" checks; genesis only, no rewarded mainnet blocks mined\n";
 } catch (...) {std::filesystem::remove_all(dir);throw;}
}
