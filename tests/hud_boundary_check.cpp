#include "../runtime/render/hud_boundary.h"
#include "../runtime/render/shader_capture.h"
#include <cassert>
#include <cstdio>
#include <limits>
int main(){
  using namespace fo2render;
  uint32_t vs[]={0xfffe0101,31,0x80000000,0x900f0000,31,0x8000000a,0x900f0001,31,0x80000005,0x900f0002,
    20,0xc00f0000,0x90e40000,0xa0e40000,1,0xd00f0000,0x90e40001,1,0xe00f0000,0x90e40002,0xffff};
  uint32_t ps[]={0xffff0101,66,0xb00f0000,5,0x800f0000,0xb0e40000,0x90e40000,0xffff};
  const auto v=shaderHash(vs,sizeof vs),p=shaderHash(ps,sizeof ps);float w[4]={0,0,0,1};
  assert(knownHudBoundary(v,sizeof vs,p,sizeof ps,w,false,true,false,true));
  uint32_t actual[]={0xfffe0101,31,0x80000000,0x900f0000,31,0x8000000a,0x900f0002,31,0x80000005,0x900f0003,
    20,0xc00f0000,0x90e40000,0xa0e40000,1,0xd00f0000,0x90e40002,1,0xe0030000,0x90540003,0xffff};
  assert(shaderHash(actual,sizeof actual)==0xe539ac0c9672e9baull);
  assert(knownHudBoundary(shaderHash(actual,sizeof actual),sizeof actual,p,sizeof ps,w,false,true,false,true));
  assert(knownHudVertex(0x119ca7f6d682b2aaull,88)&&knownHudVertex(0x891cfe0449e3e459ull,88)&&knownHudVertex(0xa98a38246889657eull,96));
  assert(!knownHudVertex(0xe539ac0c9672e9baull,88));
  assert(!knownHudBoundary(v+1,sizeof vs,p,sizeof ps,w,false,true,false,true));
  assert(!knownHudBoundary(v,sizeof vs,p+1,sizeof ps,w,false,true,false,true));
  assert(!knownHudBoundary(v,80,p,sizeof ps,w,false,true,false,true));
  assert(!knownHudBoundary(v,sizeof vs,p,28,w,false,true,false,true));
  assert(knownHudBoundary(v,sizeof vs,p,sizeof ps,w,true,true,false,true));
  assert(!knownHudBoundary(v,sizeof vs,p,sizeof ps,w,false,false,false,true));
  assert(!knownHudBoundary(v,sizeof vs,p,sizeof ps,w,false,true,true,true));
  assert(!knownHudBoundary(v,sizeof vs,p,sizeof ps,w,false,true,false,false));
  w[2]=1;assert(!knownHudBoundary(v,sizeof vs,p,sizeof ps,w,false,true,false,true));w[2]=0;
  w[3]=0;assert(!knownHudBoundary(v,sizeof vs,p,sizeof ps,w,false,true,false,true));
  w[3]=std::numeric_limits<float>::quiet_NaN();assert(!knownHudBoundary(v,sizeof vs,p,sizeof ps,w,false,true,false,true));
  std::puts("PASS: exact captured HUD shaders, orthographic matrix, blend/depth and postprocess texture exclusions");
}
