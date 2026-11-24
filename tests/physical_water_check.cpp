#include "../runtime/render/physical_water.h"
#include <cassert>
#include <cstdio>
using namespace fo2render;
int main(){
  PhysicalWaterRegion region;region.set({{{{0,2,0},{8,2,0},{0,2,8}}},{{{8,2,0},{8,2,8},{0,2,8}}}});
  assert(region.contains({2,2,2})&&region.contains({2,2.1f,2}));
  assert(!region.contains({2,3,2})&&!region.contains({20,2,2}));
  TraceMesh mesh;mesh.positions={{1,2,1},{3,2,1},{1,2,3}};mesh.indices={0,1,2};
  WaterMatrix world{};for(unsigned i=0;i<4;++i)world[i][i]=1;
  assert(region.matches(mesh,world));world[1][3]=1;assert(!region.matches(mesh,world));world[1][3]=0;
  mesh.positions={{1,1.95f,1},{3,1.95f,1},{1,2.05f,1}};assert(!region.matches(mesh,world)); // near water but upright glass.
  Draw d;d.vs=std::make_shared<Shader>();d.vs->uniforms={{0,0,8}};d.vsConstants.resize(128);d.debugProjection={{2,3,1.01f,-.1f},true};
  WaterMatrix projection={{{2,0,0,0},{0,3,0,0},{0,0,1.01f,-.1f},{0,0,1,0}}};
  auto pack=[&](const WaterMatrix& w){
    WaterMatrix vp{};for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)for(unsigned k=0;k<4;++k)vp[r][c]+=projection[r][k]*w[k][c];
    float values[32];for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c){values[r*4+c]=float(vp[r][c]);values[16+r*4+c]=float(w[r][c]);}
    std::memcpy(d.vsConstants.data(),values,128);
  };
  pack(world);auto n=physicalWaterNormal(d,waterPackedMatrix(d,4));assert(std::abs(n[1]-1)<1e-6&&std::abs(n[0])+std::abs(n[2])<1e-6);
  // Camera reconstruction cancels a translated/rotated object transform.
  world={{{1,0,0,2},{0,0,-1,3},{0,1,0,4},{0,0,0,1}}};pack(world);
  n=physicalWaterNormal(d,waterPackedMatrix(d,4));assert(std::abs(n[1]-1)<1e-6&&std::abs(n[0])+std::abs(n[2])<1e-6);
  // A mixed dry/wet batch keeps just the physical-water triangles.
  mesh.positions={{1,2,1},{3,2,1},{1,2,3},{20,2,20},{23,2,20},{20,2,23}};mesh.indices={0,1,2,3,4,5};
  WaterMatrix identity{};for(unsigned k=0;k<4;++k)identity[k][k]=1;
  assert(region.filter(mesh,identity)==std::vector<uint32_t>({0,1,2})&&!region.matches(mesh,identity));
  Draw indexed;indexed.count=6;indexed.indices=std::make_shared<Buffer>();indexed.indexSize=2;indexed.indexStart=1;indexed.baseVertex=-2;
  uint16_t raw[]={99,4,2,4,7,2,9};indexed.indices->bytes.resize(sizeof raw);std::memcpy(indexed.indices->bytes.data(),raw,sizeof raw);
  assert(traceSourceIndices(indexed,4)==std::vector<uint32_t>({2,0,5,7}));
  indexed.primitive=Primitive::TriangleStrip;indexed.count=6;uint16_t strip[]={99,4,2,0xffff,7,2,9};std::memcpy(indexed.indices->bytes.data(),strip,sizeof strip);
  assert(traceSourceIndices(indexed,4)==std::vector<uint32_t>({2,0,5,7}));
  // Baked static terrain must agree with both the supplied world eye and view direction.
  d.vs->uniforms={{0,0,4},{0,8,2}};d.vsConstants.assign(96,0);
  float baked[]={2,0,0,-6,0,3,0,-15,0,0,1.01f,6.97f,0,0,1,7,3,5,-7,1,0,0,1,0};
  std::memcpy(d.vsConstants.data(),baked,sizeof baked);assert(physicalWaterBakedWorld(d)==identity);
  baked[16]=4;std::memcpy(d.vsConstants.data(),baked,sizeof baked);bool eyeRejected=false;
  try{physicalWaterBakedWorld(d);}catch(...){eyeRejected=true;}assert(eyeRejected);
  d.vsConstants.resize(128);pack(world);d.vs->uniforms={{0,0,8}};
  d.debugProjection.values[0]=4;bool rejected=false;try{physicalWaterNormal(d,waterPackedMatrix(d,4));}catch(...){rejected=true;}assert(rejected);
  rejected=false;try{PhysicalWaterRegion invalid;invalid.set({{{{0,0,0},{10000,0,0},{0,0,10000}}}});}catch(...){rejected=true;}assert(rejected);
  std::puts("PASS: physics-water region, dry/above/vertical rejection, world transforms, camera basis, projection and index budgets");
}
