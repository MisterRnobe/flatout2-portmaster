#pragma once
#include "raytracing_fixture.h"
#include <algorithm>
#include <vector>
#include <limits>
namespace fo2render {
inline constexpr uint32_t traceSceneMaxDraws=2048,traceSceneMaxVertices=250000,traceSceneMaxTriangles=250000;
// Prepared camera-relative snapshot. Does not describe the complete world.
struct TraceScene {
  std::vector<std::array<float,3>> positions;
  std::vector<uint32_t> indices,meshIds;
  std::array<float,4> projection{};
};
inline void validateTraceScene(const TraceScene& s){
  if(s.positions.empty()||s.positions.size()>traceSceneMaxVertices||s.indices.empty()||
     s.indices.size()%3||s.indices.size()>size_t(traceSceneMaxTriangles)*3||s.meshIds.size()!=s.indices.size()/3)
    throw std::runtime_error("RT scene sizes exceed capture limits or disagree");
  for(float p:s.projection)if(!std::isfinite(p))throw std::runtime_error("Nonfinite RT scene projection");
  if(s.projection[0]<=0||s.projection[1]<=0||s.projection[2]<.99999f||s.projection[3]>=0)
    throw std::runtime_error("Invalid RT scene projection");
  for(auto p:s.positions)for(float x:p)if(!std::isfinite(x)||std::abs(x)>1e7f)
    throw std::runtime_error("Invalid RT scene position");
  for(auto i:s.indices)if(i>=s.positions.size())throw std::runtime_error("RT scene index out of range");
  for(auto i:s.meshIds)if(i>=traceSceneMaxDraws)throw std::runtime_error("RT scene mesh ID out of range");
}
// Double-precision Moller-Trumbore reference; two-sided, no face culling.
inline TraceHit traceSceneTriangle(const TraceScene& s,const TraceRay& ray,uint32_t primitive){
  TraceHit hit;
  if(!(ray.mask[0]&255))return hit;
  const auto &a=s.positions[s.indices[primitive*3]],&b=s.positions[s.indices[primitive*3+1]],&c=s.positions[s.indices[primitive*3+2]];
  double e1[3],e2[3],p[3],t[3],q[3];
  for(unsigned k=0;k<3;++k){e1[k]=double(b[k])-a[k];e2[k]=double(c[k])-a[k];t[k]=double(ray.originMin[k])-a[k];}
  auto cross=[](const auto* a,const auto* b,double* out){for(unsigned k=0;k<3;++k)out[k]=a[(k+1)%3]*b[(k+2)%3]-a[(k+2)%3]*b[(k+1)%3];};
  cross(ray.directionMax.data(),e2,p);
  double determinant=0,scale=0;
  for(unsigned k=0;k<3;++k){determinant+=e1[k]*p[k];scale+=e1[k]*e1[k];}
  double pLength=0;for(double x:p)pLength+=x*x;
  if(std::abs(determinant)<=1e-12*std::sqrt(scale*pLength))return hit;
  double u=0;for(unsigned k=0;k<3;++k)u+=t[k]*p[k];u/=determinant;
  if(u<0||u>1)return hit;
  cross(t,e1,q);
  double v=0,distance=0;
  for(unsigned k=0;k<3;++k){v+=ray.directionMax[k]*q[k];distance+=e2[k]*q[k];}
  v/=determinant;distance/=determinant;
  if(v<0||u+v>1||distance<ray.originMin[3]||distance>ray.directionMax[3]||!std::isfinite(distance))return hit;
  hit.valid=1;hit.primitive=primitive;hit.instance=0;hit.distance=float(distance);hit.barycentric={float(u),float(v),0,0};return hit;
}
inline TraceHit traceSceneReference(const TraceScene& s,const TraceRay& ray){
  if(!validTraceRay(ray))throw std::runtime_error("Invalid RT scene ray");
  TraceHit nearest;if(!(ray.mask[0]&255))return nearest;
  for(uint32_t p=0;p<s.indices.size()/3;++p){auto hit=traceSceneTriangle(s,ray,p);
    if(hit.valid&&(!nearest.valid||hit.distance<nearest.distance))nearest=hit;}
  return nearest;
}
inline bool traceSceneBarycentricDisagrees(const TraceHit& a,const TraceHit& b){
  for(unsigned k=0;k<2;++k)if(!std::isfinite(a.barycentric[k])||std::abs(a.barycentric[k]-b.barycentric[k])>.002f)return true;
  return false;
}
// Narrow numerical equivalence for the same primitive at grazing incidence.
// This is a local validation policy, not a Metal accuracy guarantee. Reconstruct
// the GPU point and require its ray residual to fit an eight-operation float32
// rounding envelope. Grazing amplification may move the exact intersection
// along the surface, but never beyond the existing nearest-distance tolerance.
inline bool traceScenePrecisionEquivalent(const TraceScene& s,const TraceRay& ray,const TraceHit& actual,const TraceHit& nearest,const TraceHit& own){
  if(!actual.valid||!nearest.valid||!own.valid||actual.instance!=0||actual.primitive!=nearest.primitive||actual.primitive>=s.indices.size()/3)return false;
  double u=actual.barycentric[0],v=actual.barycentric[1];
  if(!std::isfinite(u)||!std::isfinite(v)||u<0||v<0||u+v>1||!std::isfinite(actual.distance))return false;
  const auto &a=s.positions[s.indices[actual.primitive*3]],&b=s.positions[s.indices[actual.primitive*3+1]],&c=s.positions[s.indices[actual.primitive*3+2]];
  double e1[3],e2[3],n[3],directionLength2=0,normalLength2=0,dot=0;
  for(unsigned k=0;k<3;++k){e1[k]=double(b[k])-a[k];e2[k]=double(c[k])-a[k];directionLength2+=double(ray.directionMax[k])*ray.directionMax[k];}
  for(unsigned k=0;k<3;++k){n[k]=e1[(k+1)%3]*e2[(k+2)%3]-e1[(k+2)%3]*e2[(k+1)%3];normalLength2+=n[k]*n[k];dot+=n[k]*ray.directionMax[k];}
  if(directionLength2<=0||normalLength2<=0)return false;
  double cosine=std::abs(dot)/std::sqrt(directionLength2*normalLength2);
  if(!std::isfinite(cosine)||cosine<=0||cosine>=.01)return false;
  double envelope2=0,gap2=0;
  for(unsigned k=0;k<3;++k){
    double gpuPoint=a[k]+u*e1[k]+v*e2[k];
    double exactPoint=a[k]+double(own.barycentric[0])*e1[k]+double(own.barycentric[1])*e2[k];
    double rayPoint=ray.originMin[k]+double(actual.distance)*ray.directionMax[k];
    double magnitude=std::abs(double(a[k]))+std::abs(u*e1[k])+std::abs(v*e2[k])+std::abs(double(ray.originMin[k]))+std::abs(double(actual.distance)*ray.directionMax[k]);
    double envelope=8*std::numeric_limits<float>::epsilon()*magnitude+8*double(std::numeric_limits<float>::denorm_min());
    if(!std::isfinite(gpuPoint)||!std::isfinite(rayPoint)||std::abs(gpuPoint-rayPoint)>envelope)return false;
    envelope2+=envelope*envelope;gap2+=(gpuPoint-exactPoint)*(gpuPoint-exactPoint);
  }
  double distanceTolerance=.001f+.0001f*std::abs(nearest.distance);
  if(std::abs(double(actual.distance)-nearest.distance)>distanceTolerance||std::abs(double(actual.distance)-own.distance)>distanceTolerance)return false;
  double allowed=std::min(std::sqrt(envelope2)/cosine,distanceTolerance*std::sqrt(directionLength2));
  return std::isfinite(gap2)&&std::sqrt(gap2)<=allowed;
}
inline const char* traceSceneHitMismatch(const TraceScene& s,const TraceRay& ray,const TraceHit& actual,const TraceHit& nearest){
  if(actual.valid!=nearest.valid)return "hit-miss";
  if(!nearest.valid)return traceHitMatches(actual,nearest)?nullptr:"invalid-miss";
  if(actual.instance!=0||actual.primitive>=s.indices.size()/3||!std::isfinite(actual.distance))return "invalid-hit";
  // Coplanar duplicate triangles can tie. Validate the reported primitive and
  // its barycentrics independently, while still requiring nearest distance.
  auto own=traceSceneTriangle(s,ray,actual.primitive);
  if(!own.valid)return "reported-primitive-misses-exact-ray";
  float tolerance=.001f+.0001f*std::abs(nearest.distance);
  if(std::abs(actual.distance-nearest.distance)>tolerance)return "nearest-distance";
  if(std::abs(actual.distance-own.distance)>tolerance)return "primitive-distance";
  if(traceSceneBarycentricDisagrees(actual,own)&&!traceScenePrecisionEquivalent(s,ray,actual,nearest,own))return "barycentric";
  return nullptr;
}
inline bool traceSceneHitMatches(const TraceScene& s,const TraceRay& ray,const TraceHit& actual,const TraceHit& nearest){return traceSceneHitMismatch(s,ray,actual,nearest)==nullptr;}
inline TraceRay traceCameraRay(const TraceScene& scene,unsigned x,unsigned y,unsigned width,unsigned height){
  TraceRay r{};r.directionMax={((x+.5f)/width*2-1)/scene.projection[0],(1-(y+.5f)/height*2)/scene.projection[1],-1,1000000};
  float length=std::sqrt(r.directionMax[0]*r.directionMax[0]+r.directionMax[1]*r.directionMax[1]+1);
  for(unsigned k=0;k<3;++k)r.directionMax[k]/=length;
  r.originMin[3]=.001f;r.mask[0]=255;return r;
}
inline std::vector<TraceRay> traceSceneProbes(const TraceScene& scene){
  std::vector<TraceRay> rays;
  for(unsigned y=0;y<4;++y)for(unsigned x=0;x<8;++x)rays.push_back(traceCameraRay(scene,x,y,8,4));
  auto count=scene.indices.size()/3;
  for(unsigned i=0;i<std::min(size_t(32),count);++i){
    auto primitive=i*count/std::min(size_t(32),count);TraceRay r{};
    const float weights[3]={.27f,.31f,.42f};float length=0;
    for(unsigned k=0;k<3;++k){for(unsigned j=0;j<3;++j)r.directionMax[k]+=weights[j]*scene.positions[scene.indices[primitive*3+j]][k];length+=r.directionMax[k]*r.directionMax[k];}
    length=std::sqrt(length);if(length<.01f)continue;
    for(unsigned k=0;k<3;++k)r.directionMax[k]/=length;
    r.originMin[3]=.001f;r.directionMax[3]=length+1;r.mask[0]=255;rays.push_back(r);
    auto masked=r;masked.mask[0]=0;rays.push_back(masked);
  }
  return rays;
}
}
