#include "qbtc/wallet.hpp"
#include <filesystem>
#include <iostream>
#include <fstream>
using namespace qbtc;
int main(int argc,char **argv){
 if((argc==4 || argc==5) && std::string_view(argv[1])=="verify") {
  auto chain=Chain::load(Network::Regtest,argv[2]);auto tx=decode_transaction(wallet::read_bounded(argv[3],MAX_BLOCK_BYTES));
  Address recipient{};recipient[0]=7;
  if(tx.outputs.empty()||tx.outputs[0].owner!=recipient||tx.outputs[0].value!=COIN)throw Invalid("restored payment intent mismatch");
  auto b=candidate(chain,Address{},chain.tip_header().timestamp+600,{tx});mine(b,100000);chain.add(b);
  if(argc==5)chain.save(argv[4]);
  std::cout<<"PASS restored payment independently validated and included in a disposable Regtest block\n";return 0;
 }
 if(argc!=2)return 2;
 auto dir=std::filesystem::path(argv[1]);
 auto key=wallet::Key::generate();key.save((dir/"fixture.qwlt").string(),"Fixture-only-password-2026!");
 key.save((dir/"backup.qwlt").string(),"Fixture-only-password-2026!");
 std::ofstream(dir/"owner.txt")<<wallet::address(key.owner(),Network::Regtest);
 Address recipient{};recipient[0]=7;Chain chain(Network::Regtest);
 for(unsigned i=0;i<113;i++){auto b=candidate(chain,key.owner(),chain.tip_header().timestamp+600);mine(b,100000);chain.add(b);}
 chain.append((dir/"chain.db").string());
 std::cout<<wallet::address(recipient,Network::Regtest)<<"\n";
}
