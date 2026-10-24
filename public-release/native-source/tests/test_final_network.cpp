#include "qbtc/consensus.hpp"
#include "qbtc/storage.hpp"
#include <iostream>
#include <filesystem>
using namespace qbtc;
int main(){
 auto require=[](bool x,const char*m){if(!x)throw Invalid(m);};
 require(static_cast<std::uint32_t>(ACTIVE_NETWORK)==0x51424d33,"final network ID");
 auto g=genesis(ACTIVE_NETWORK);Chain c(ACTIVE_NETWORK);
 require(hex(blockid(g.header))=="0000000098640dbb2dfc0f2daad73b786e7f0c823ace8c9f160688dd624032354fa284ef727ec32e9e6b46a6364deb403407a22f9fa63aeca778b33a609e17fc","frozen named genesis");
 require(hex(g.header.root)=="057160133527c1de40568ba744bb508ca11e7527415a6592f2c3529c3bf328d5dc722512123eb5995696e1c260d0ff9d80a09b9cd3d844e601a74108af05dca0","name commitment");
 require(g.transactions.empty() && c.utxos().empty() && subsidy(0)==0,"no allocations");
 require(hex(g.header.target)=="0000000388804cf9952cfc713652bd74be16039e3d1a5328b1c43652d7f48057537d0e7fa4401e5121e47d0fa32198ecbfb4e3d78ba4cba70c01cd0e563ca097" && c.expected_target(blockid(g.header))==g.header.target,"difficulty from genesis");
 require(valid_pow(g.header) && g.header.nonce==635623644ULL,"genesis PoW");
 require(SPACING==600 && INTERVAL==2016 && HALVING==210000 && subsidy(1)==50*COIN,"economic rules");
 bool rejected=false;try{Chain old(static_cast<Network>(0x51424d32));}catch(const Invalid&){rejected=true;}
 require(rejected,"test-v2 identity rejected");
 auto path=(std::filesystem::temp_directory_path()/"qbtc-final-origin-check.db").string();
 if(std::filesystem::exists(path))throw Invalid("fixture already exists");
 c.append(path);auto restored=Chain::load(ACTIVE_NETWORK,path);
 require(blockid(restored.tip_header())==blockid(g.header) && restored.utxos().empty(),"height-zero restart");
 std::filesystem::remove(path);
 std::cout<<"PASS final named genesis, PoW, 2.026 MH/s initial target, 600s spacing, 2016-block retarget, zero allocations, test network rejection and height-zero restart; no rewarded final blocks mined\n";
}
