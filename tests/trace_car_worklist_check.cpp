#include "../runtime/render/trace_car_worklist.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <random>
using namespace fo2render;
int main(){
  auto owners=traceCarPrimitiveOwners(9,{{4,2},{0,1},{8,1}});
  assert(owners[0]==1&&owners[4]==0&&owners[5]==0&&owners[8]==2);
  for(unsigned i:{1,2,3,6,7})assert(owners[i]==traceCarNoOwner);
  for(auto ranges:std::vector<std::vector<std::array<uint32_t,2>>>{{{0,0}},{{9,1}},{{8,2}},{{0,2},{1,2}},{{UINT32_MAX,2}}}){
    bool rejected=false;try{traceCarPrimitiveOwners(9,ranges);}catch(...){rejected=true;}assert(rejected);
  }
  bool rejected=false;try{traceCarPrimitiveOwners(9,std::vector<std::array<uint32_t,2>>(65,{0,1}));}catch(...){rejected=true;}assert(rejected);
  // Randomized disjoint ranges in arbitrary capture order must preserve exact
  // ownership, gaps and first/last primitive boundaries across all 64 slots.
  std::mt19937 random(417);
  for(unsigned run=0;run<100;++run){
    std::vector<std::array<uint32_t,2>> ranges;std::vector<uint32_t> expected(250000,traceCarNoOwner);
    for(unsigned i=0;i<64;++i){uint32_t first=i*3000+random()%100,count=1+random()%2000;ranges.push_back({first,count});}
    std::shuffle(ranges.begin(),ranges.end(),random);
    for(unsigned i=0;i<64;++i)for(uint32_t j=ranges[i][0];j<ranges[i][0]+ranges[i][1];++j)expected[j]=i;
    assert(traceCarPrimitiveOwners(250000,ranges)==expected);
  }
  assert(traceCarWorklistBytes(100,64000,64)==100*4+64000*4+64*36);
  assert(traceCarWorklistBytes(250000,2048*2048,64)<18*1024*1024);
  for(auto args:std::vector<std::array<size_t,3>>{{0,1,1},{250001,1,1},{1,0,1},{1,2048*2048+1,1},{1,1,65}}){
    rejected=false;try{traceCarWorklistBytes(args[0],args[1],args[2]);}catch(...){rejected=true;}assert(rejected);
  }
  std::puts("RT car worklist: material ownership, unknown gaps, overflow/overlap rejection, 64 shuffled ranges and allocation limits passed");
}
