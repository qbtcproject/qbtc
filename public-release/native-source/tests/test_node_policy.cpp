#define main qbtc_peer_cli_main
#include "../src/peer_main.cpp"
#undef main
#include <openssl/rand.h>
#include "qbtc/wallet.hpp"
void check_policy(bool condition,const char *message){if(!condition)throw Invalid(message);}
int main(){
 Bytes salt(16);if(RAND_bytes(salt.data(),16)!=1)return 2;
 auto folder=std::filesystem::temp_directory_path()/("qbtc-node-policy-"+hex(salt));std::filesystem::create_directory(folder);
 try{
  Node node((folder/"chain.db").string());auto genesis_id=blockid(node.chain().tip_header());
  Bytes locator;put32(locator,1);locator.insert(locator.end(),genesis_id.begin(),genesis_id.end());
  auto before=node.handle('B',locator);check_policy(Reader{before}.u32()==0,"genesis cache response");
  Bytes mine_body(32);put32(mine_body,2);node.handle('M',mine_body);
  auto after=node.handle('B',locator);check_policy(Reader{after}.u32()==2,"cache did not follow mining tip");
  Chain fork(Network::Regtest);Address owner{};owner[0]=7;Bytes blocks;put32(blocks,4);
  for(unsigned i=0;i<4;i++){auto b=candidate(fork,owner,fork.tip_header().timestamp+600);mine(b,100000);fork.add(b);auto raw=encode(b);put32(blocks,static_cast<std::uint32_t>(raw.size()));blocks.insert(blocks.end(),raw.begin(),raw.end());}
  Hash last{};Bytes pool;put32(pool,0);node.import(blocks,pool,last);
  auto reorganized=node.handle('B',locator);check_policy(Reader{reorganized}.u32()==4,"cache did not follow reorganization");
  check_policy(blockid(node.chain().tip_header())==blockid(fork.tip_header()),"fork tip mismatch");
  auto l=node.locator(Hash{});Reader lr{l};check_policy(lr.u32()>0&&lr.id()==blockid(fork.tip_header()),"locator retained stale tip");
  auto key=wallet::Key::generate();auto own=key.owner();
  Bytes rewards(own.begin(),own.end());put32(rewards,100);node.handle('M',rewards);
  auto payment=wallet::payment(key,node.chain(),own,COIN,1000);node.admit(payment);
  for(unsigned i=0;i<20;i++)node.admit(payment);
  Bytes next(own.begin(),own.end());put32(next,1);node.handle('M',next);
  bool spent_rejected=false;try{node.admit(payment);}catch(const Invalid &){spent_rejected=true;}
  check_policy(spent_rejected,"confirmed transaction stayed in duplicate cache");
  // Novel malformed transactions consume a shared finite budget before expensive proposal work.
  Transaction tx;tx.inputs.push_back({{},Bytes(1952),Bytes(3309)});tx.outputs.push_back({1,{}});
  bool limited=false;
  for(unsigned i=0;i<20;i++){tx.inputs[0].previous.index=i;try{node.admit(tx);}catch(const Invalid &e){if(std::string(e.what()).find("rate limit")!=std::string::npos)limited=true;}}
  check_policy(limited&&node.chain().tip_header().height==105,"admission flood unbounded or changed chain");
  std::cout<<"PASS node policy: cache follows mining and reorganization; locator updates; novel transaction flood rate-limited without chain mutation\n";
 }catch(...){std::filesystem::remove_all(folder);throw;}
 std::filesystem::remove_all(folder);
}
