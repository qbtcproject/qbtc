#include "qbtc/mempool.hpp"
#include "qbtc/wallet.hpp"
#include <iostream>
using namespace qbtc;
void check(bool b,const char *m){if(!b)throw Invalid(m);}
template<class F> void rejects(F f){bool bad=false;try{f();}catch(const Invalid &){bad=true;}check(bad,"relay accepted forbidden structure");}
int main(){
 Transaction t;t.inputs.push_back({{},Bytes(1952),Bytes(3309)});t.outputs.push_back({1,{}});
 mempool::check_admission(t);
 auto bad=t;bad.inputs[0].signature.clear();rejects([&]{mempool::check_admission(bad);});
 bad=t;bad.inputs[0].public_key.resize(1953);rejects([&]{mempool::check_admission(bad);});
 bad=t;bad.inputs.resize(33,t.inputs[0]);rejects([&]{mempool::check_admission(bad);});
 bad=t;bad.outputs.resize(129,t.outputs[0]);rejects([&]{mempool::check_admission(bad);});
 bad=t;bad.coinbase_height=1;rejects([&]{mempool::check_admission(bad);});
 bad=t;bad.outputs.clear();rejects([&]{mempool::check_admission(bad);});
 // A transaction forbidden by relay policy must remain valid in a mined block.
 auto key=wallet::Key::generate();Chain chain(Network::Regtest);
 for(unsigned i=0;i<113;i++){auto b=candidate(chain,key.owner(),chain.tip_header().timestamp+600);mine(b,100000);chain.add(b);}
 rejects([&]{(void)wallet::payment(key,chain,key.owner(),650*COIN,0,true);});
 auto small=wallet::payment(key,chain,key.owner(),COIN,1000,true);mempool::check_admission(small);
 auto payment=wallet::payment(key,chain,key.owner(),650*COIN,0);
 check(encode(payment).size()>mempool::MAX_RELAY_BYTES,"large signed fixture");
 rejects([&]{mempool::check_admission(payment);});
 auto block=candidate(chain,key.owner(),chain.tip_header().timestamp+600,{payment});mine(block,100000);chain.add(block);
 check(chain.tip_header().height==114,"relay policy altered block consensus");
 std::cout<<"PASS relay rejects malformed/oversized transactions before cryptography; signed over-policy transaction remains consensus-valid in block\n";
}
