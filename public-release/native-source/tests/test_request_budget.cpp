#include "qbtc/ratelimit.hpp"
#include <iostream>
using namespace qbtc;
void check(bool b){if(!b)throw Invalid("request-budget regression");}
int main(){
 using Clock=std::chrono::steady_clock;auto t=Clock::time_point{};
 RequestBudget budget(8,2,t);
 for(unsigned i=0;i<8;i++)check(budget.take(t));
 check(!budget.take(t));check(!budget.take(t+std::chrono::milliseconds(499)));
 check(budget.take(t+std::chrono::milliseconds(500)));check(!budget.take(t+std::chrono::milliseconds(500)));
 check(!budget.take(t-std::chrono::seconds(1)));
 for(unsigned i=0;i<8;i++)check(budget.take(t+std::chrono::hours(24)));
 check(!budget.take(t+std::chrono::hours(24)));
 bool rejected=false;try{RequestBudget zero(0,1,t);}catch(const Invalid &){rejected=true;}check(rejected);
 std::cout<<"PASS request budget: burst bound, fractional refill, backward time, long-idle cap, invalid configuration\n";
}
