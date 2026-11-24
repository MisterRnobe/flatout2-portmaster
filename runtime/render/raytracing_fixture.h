#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
namespace fo2render {
// Shared host/MSL layout; no guest addresses or SIMD ABI-dependent float3.
struct alignas(16) TraceRay {
  std::array<float,4> originMin, directionMax;
  std::array<uint32_t,4> mask;
};
struct alignas(16) TraceHit {
  float distance=0;
  uint32_t primitive=UINT32_MAX,instance=UINT32_MAX,valid=0;
  std::array<float,4> barycentric{};
};
static_assert(sizeof(TraceRay)==48 && offsetof(TraceRay,mask)==32);
static_assert(sizeof(TraceHit)==32 && offsetof(TraceHit,barycentric)==16);
inline constexpr float traceVertices[4][4]={{99,99,99,99},{-1,-1,0,7},{1,-1,0,7},{0,1,0,7}};
inline constexpr uint16_t traceIndices16[]={0xffff,0xffff,0,1,2,0xffff};
inline constexpr uint32_t traceIndices32[]={0xffffffff,0,1,2};
inline std::array<TraceRay,9> traceRays(){
  return {{{{0,0,-2,0},{0,0,1,10},{3,0,0,0}},
           {{0,0,-2,0},{0,0,1,10},{2,0,0,0}},
           {{3,0,-2,0},{0,0,1,10},{3,0,0,0}},
           {{0,0,-2,0},{0,0,1,1},{3,0,0,0}},
           {{0,0,-2,2.5f},{0,0,1,10},{3,0,0,0}},
           {{0,0,3,0},{0,0,-1,10},{3,0,0,0}},
           {{0,0,-2,0},{0,0,1,10},{0,0,0,0}},
           {{0,0,-2,0},{1,0,0,10},{3,0,0,0}},
           {{0,0,-2,0},{0,0,1,10},{1,0,0,0}}}};
}
inline bool validTraceRay(const TraceRay& r){
  for(float v:r.originMin)if(!std::isfinite(v))return false;
  for(float v:r.directionMax)if(!std::isfinite(v))return false;
  double length=0;for(unsigned i=0;i<3;++i)length+=double(r.directionMax[i])*r.directionMax[i];
  return length>1e-20 && r.originMin[3]>=0 && r.directionMax[3]>=r.originMin[3];
}
// Independent double-precision reference for the two translated triangle instances.
inline TraceHit traceReference(const TraceRay& r){
  if(!validTraceRay(r))throw std::runtime_error("Invalid ray tracing fixture ray");
  TraceHit hit;double closest=r.directionMax[3];
  for(uint32_t instance=0;instance<2;++instance){
    if(!(r.mask[0]&(1u<<instance)) || std::abs(r.directionMax[2])<1e-12)continue;
    double t=(2.*instance-r.originMin[2])/r.directionMax[2];
    if(t<r.originMin[3]||t>closest)continue;
    double x=r.originMin[0]+t*r.directionMax[0],y=r.originMin[1]+t*r.directionMax[1];
    double v=(y+1)/2,u=(x+1-v)/2;
    if(u<0||v<0||u+v>1)continue;
    closest=t;hit.distance=float(t);hit.primitive=0;hit.instance=instance;hit.valid=1;
    hit.barycentric={float(u),float(v),0,0};
  }
  return hit;
}
inline bool traceHitMatches(const TraceHit& actual,const TraceHit& expected){
  if(actual.valid!=expected.valid)return false;
  if(!expected.valid)return actual.primitive==UINT32_MAX && actual.instance==UINT32_MAX &&
    actual.distance==0 && actual.barycentric==std::array<float,4>{};
  return actual.primitive==expected.primitive && actual.instance==expected.instance &&
    std::isfinite(actual.distance)&&std::abs(actual.distance-expected.distance)<2e-4f &&
    std::isfinite(actual.barycentric[0])&&std::isfinite(actual.barycentric[1])&&
    std::abs(actual.barycentric[0]-expected.barycentric[0])<2e-4f&&
    std::abs(actual.barycentric[1]-expected.barycentric[1])<2e-4f;
}
inline void validateTraceFixture(){
  auto rays=traceRays();
  const std::array<int,9> instances={0,1,-1,-1,1,1,-1,-1,0};
  const std::array<float,9> distances={2,4,0,0,4,1,0,0,2};
  for(unsigned i=0;i<rays.size();++i){
    TraceHit expected;
    if(instances[i]>=0){expected.valid=1;expected.instance=uint32_t(instances[i]);expected.primitive=0;
      expected.distance=distances[i];expected.barycentric={.25f,.5f,0,0};}
    if(!traceHitMatches(traceReference(rays[i]),expected))throw std::runtime_error("CPU ray tracing fixture mismatch");
  }
}
}
