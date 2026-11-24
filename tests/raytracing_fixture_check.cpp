#include "../runtime/render/raytracing_fixture.h"
#include <cassert>
#include <cstdio>
#include <limits>
int main(){
  using namespace fo2render;
  validateTraceFixture();auto rays=traceRays();
  auto invalid=rays[0];invalid.originMin[0]=std::numeric_limits<float>::quiet_NaN();assert(!validTraceRay(invalid));
  invalid=rays[0];invalid.directionMax[2]=0;assert(!validTraceRay(invalid));
  invalid=rays[0];invalid.directionMax[3]=std::numeric_limits<float>::infinity();assert(!validTraceRay(invalid));
  invalid=rays[0];invalid.originMin[3]=-1;assert(!validTraceRay(invalid));
  invalid=rays[0];invalid.originMin[3]=11;assert(!validTraceRay(invalid));
  bool rejected=false;try{traceReference(invalid);}catch(...){rejected=true;}assert(rejected);
  auto expected=traceReference(rays[0]),wrong=expected;wrong.instance=1;assert(!traceHitMatches(wrong,expected));
  wrong=expected;wrong.distance=std::numeric_limits<float>::quiet_NaN();assert(!traceHitMatches(wrong,expected));
  wrong=expected;wrong.barycentric[0]+=.1f;assert(!traceHitMatches(wrong,expected));
  TraceHit miss;wrong=miss;wrong.primitive=0;assert(!traceHitMatches(wrong,miss));
  std::puts("PASS: ray ABI, nearest/transformed/backface hits, masks, range/parallel misses, barycentrics and invalid input/results");
}
