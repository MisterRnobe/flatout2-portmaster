#include "../runtime/render/trace_scene.h"
#include <cassert>
#include <cstdio>
#include <limits>
int main(){
  using namespace fo2render;
  TraceScene s;s.projection={1,1,1.01f,-.1f};
  s.positions={{{-1,-1,-2}},{{1,-1,-2}},{{0,1,-2}},{{-1,-1,-4}},{{1,-1,-4}},{{0,1,-4}}};
  s.indices={0,1,2,3,4,5};s.meshIds={0,1};validateTraceScene(s);
  TraceRay r{{0,0,0,0},{0,0,-1,10},{255,0,0,0}};
  auto h=traceSceneReference(s,r);assert(h.valid&&h.primitive==0&&h.distance==2);
  assert(std::abs(h.barycentric[0]-.25f)<1e-6f&&std::abs(h.barycentric[1]-.5f)<1e-6f);
  assert(traceSceneHitMatches(s,r,h,h));
  auto wrong=h;wrong.distance=3;assert(!traceSceneHitMatches(s,r,wrong,h));
  assert(std::string(traceSceneHitMismatch(s,r,wrong,h))=="nearest-distance");
  wrong=h;wrong.barycentric[0]+=.003f;assert(std::string(traceSceneHitMismatch(s,r,wrong,h))=="barycentric");
  wrong=h;wrong.barycentric[0]+=.001f;assert(traceSceneHitMatches(s,r,wrong,h));
  wrong=h;wrong.primitive=1;assert(!traceSceneHitMatches(s,r,wrong,h));
  wrong=h;wrong.barycentric[0]=std::numeric_limits<float>::quiet_NaN();assert(!traceSceneHitMatches(s,r,wrong,h));
  r.originMin[3]=2.1f;assert(traceSceneReference(s,r).primitive==1);
  r.directionMax[3]=3;assert(!traceSceneReference(s,r).valid);
  r.originMin[3]=0;r.directionMax[3]=10;r.mask[0]=256;assert(!traceSceneReference(s,r).valid);
  r.mask[0]=0;assert(!traceSceneReference(s,r).valid);
  r.mask[0]=255;r.originMin[0]=5;assert(!traceSceneReference(s,r).valid);
  r.originMin={0,0,-5,0};r.directionMax={0,0,1,10};assert(traceSceneReference(s,r).primitive==1); // backface
  r.originMin={0,0,0,0};r.directionMax={0,0,-1,10};
  s.indices.insert(s.indices.end(),{0,1,2});s.meshIds.push_back(2);
  h=traceSceneReference(s,r);auto tie=h;tie.primitive=2;assert(traceSceneHitMatches(s,r,tie,h));
  s.positions[3]={0,0,-2};s.positions[4]={0,0,-2};s.positions[5]={0,0,-2};assert(!traceSceneTriangle(s,r,1).valid);
  auto probes=traceSceneProbes(s);assert(probes.size()>=32);
  for(auto ray:probes){assert(validTraceRay(ray));assert(traceSceneHitMatches(s,ray,traceSceneReference(s,ray),traceSceneReference(s,ray)));}
  auto camera=traceCameraRay(s,0,0,1,1);assert(camera.directionMax[0]==0&&camera.directionMax[1]==0&&camera.directionMax[2]==-1);
  auto reject=[&](auto change){auto bad=s;change(bad);bool caught=false;try{validateTraceScene(bad);}catch(...){caught=true;}assert(caught);};
  reject([](auto& s){s.indices[0]=UINT32_MAX;});reject([](auto& s){s.indices.pop_back();});
  reject([](auto& s){s.meshIds.pop_back();});reject([](auto& s){s.meshIds[0]=traceSceneMaxDraws;});
  reject([](auto& s){s.projection[0]=0;});reject([](auto& s){s.positions[0][0]=std::numeric_limits<float>::infinity();});
  bool caught=false;r.directionMax[3]=-1;try{traceSceneReference(s,r);}catch(...){caught=true;}assert(caught);
  // Real M3 Pro result: the ray grazes one narrow triangle at ~130 units.
  // CPU/GPU agree on primitive/distance; barycentrics differ by ~0.0042 but
  // reconstructed points are only ~0.000237 units apart along the surface.
  for(float scale:{1.f/1024,1.f,1024.f}){
    TraceScene grazing;grazing.projection={1,1,1.01f,-.1f};
    grazing.positions={{{-45.753849029541016f,22.59868812561035f,-120.13239288330078f}},
      {{-44.710086822509766f,22.7388973236084f,-120.63285064697266f}},
      {{-44.73505783081055f,22.74774932861328f,-120.68244934082031f}}};
    for(auto& p:grazing.positions)for(auto& x:p)x*=scale;grazing.indices={0,1,2};grazing.meshIds={0};
    TraceRay ray{{0,0,0,.001f*scale},{-.3444901406764984f,.17380332946777344f,-.9225611090660095f,131.634765625f*scale},{255,0,0,0}};
    auto reference=traceSceneReference(grazing,ray);assert(reference.valid);
    TraceHit gpu;gpu.valid=1;gpu.primitive=gpu.instance=0;gpu.distance=130.6346893310547f*scale;gpu.barycentric={.31149575114250183f,.41849610209465027f,0,0};
    assert(traceSceneBarycentricDisagrees(gpu,reference));assert(traceSceneHitMatches(grazing,ray,gpu,reference));
    auto bad=gpu;bad.barycentric[0]+=.1f;assert(!traceSceneHitMatches(grazing,ray,bad,reference));
    bad=gpu;bad.barycentric[0]=-.001f;assert(!traceSceneHitMatches(grazing,ray,bad,reference));
    bad=gpu;bad.distance+=1.f*scale;assert(!traceSceneHitMatches(grazing,ray,bad,reference));
    bad=gpu;bad.instance=1;assert(!traceSceneHitMatches(grazing,ray,bad,reference));
    bad=gpu;bad.primitive=1;assert(!traceSceneHitMatches(grazing,ray,bad,reference));
    bad=gpu;bad.barycentric[1]=std::numeric_limits<float>::quiet_NaN();assert(!traceSceneHitMatches(grazing,ray,bad,reference));
  }
  std::puts("PASS: RT scene nearest/backface/masked/ranged hits, coplanar ties, barycentrics, degenerates, camera rays and invalid input");
}
