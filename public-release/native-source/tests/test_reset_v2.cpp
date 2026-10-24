#include "qbtc/consensus.hpp"
#include <iostream>
using namespace qbtc;
int main(){
 Chain c(Network::Mainnet);auto start=blockid(c.tip_header());auto target=c.expected_target(start);
 if(target==pow_limit(Network::Mainnet))throw Invalid("uncalibrated target");
 auto b=candidate(c,Address{},c.tip_header().timestamp+600);b.header.target=pow_limit(Network::Mainnet);
 if(!mine(b,100000000))throw Invalid("fixture proof unavailable");
 bool reject=false;try{c.add(b);}catch(const Invalid &e){reject=std::string(e.what())=="unexpected target";}
 if(!reject||blockid(c.tip_header())!=start||!c.utxos().empty())throw Invalid("wrong-target acceptance or mutation");
 if(subsidy(1)!=50*COIN||HALVING!=210000||SPACING!=600||INTERVAL!=2016)throw Invalid("economic regression");
 std::cout<<"PASS reset v2: calibrated first target; old easy target rejected despite valid proof; no state mutation; economics unchanged\n";
}
