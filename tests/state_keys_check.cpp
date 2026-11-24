#include "../runtime/render/state_keys.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <random>
#include <sstream>
#include <unordered_map>
using namespace fo2render;
static std::string oldPipeline(const Draw& d,const Shader& ps,uint32_t c,uint32_t z,uint32_t m) {
  const auto&r=d.raster;std::ostringstream k;
  k<<pipelineShaderIdentity(*d.vs)<<':'<<pipelineShaderIdentity(ps)<<':'<<c<<':'<<z<<':'<<r.blendOn<<':'
   <<int(r.src)<<':'<<int(r.dst)<<':'<<int(r.srcAlpha)<<':'<<int(r.dstAlpha)<<':'
   <<int(r.blend)<<':'<<int(r.blendAlpha)<<':'<<r.colorMask<<":material="<<m;
  for(auto a:d.attributes) k<<':'<<a.slot<<','<<a.stream<<','<<a.offset<<','<<int(a.type)<<','
    <<d.strides[a.stream]<<','<<((d.perInstanceMask>>a.stream)&1)<<','<<d.stepRates[a.stream];
  return k.str();
}
int main() {
  Draw d; d.vs=std::make_shared<Shader>();d.vs->vertex=true;d.vs->hash=123;d.vs->entry="vs";
  Shader ps;ps.hash=123;ps.entry="ssrPS";
  d.attributes={{0,0,0,Element::Float3},{1,0,12,Element::Float2}};d.strides[0]=20;
  auto key=[&]{return pipelineStateKey(d,ps,80,252,70);};auto baseline=key();
  ps.entry="ssrCompositePS";assert(key()!=baseline);ps.entry="ssrPS";
  ps.vertex=true;assert(key()!=baseline);ps.vertex=false;
  d.vs->hash+=1ull<<32;assert(key()!=baseline);d.vs->hash=123;
  assert(pipelineStateKey(d,ps,81,252,70)!=baseline);
  assert(pipelineStateKey(d,ps,80,253,70)!=baseline);
  assert(pipelineStateKey(d,ps,80,252,71)!=baseline);
  d.offsets[0]=128;d.raster.stencilRef=12;assert(key()==baseline);
  Raster r;auto depth=depthStateKey(r);r.stencilRef=1;assert(depthStateKey(r)==depth);
  r.twoSidedStencil=true;auto two=depthStateKey(r);r.backStencilPass=StencilOp::Replace;
  assert(depthStateKey(r)!=two);r.twoSidedStencil=false;assert(depthStateKey(r)==depth);
  Sampler sm;auto sampler=samplerStateKey(sm);sm.lodBias=2;assert(samplerStateKey(sm)==sampler);
  sm.minLod=.5f;assert(samplerStateKey(sm)!=sampler);sm.minLod=0;
  sm.maxAnisotropy=4;assert(samplerStateKey(sm)!=sampler);sm.maxAnisotropy=1;
  for(unsigned axis=0;axis<3;++axis){sm.address[axis]++;assert(samplerStateKey(sm)!=sampler);sm.address[axis]--;}
  sm.mipEnabled=true;assert(samplerStateKey(sm)!=sampler);
  auto pipelineChange=[&](auto change){Draw copy=d;change(copy);assert(pipelineStateKey(copy,ps,80,252,70)!=baseline);};
  pipelineChange([](Draw& x){x.raster.blendOn=true;});
  pipelineChange([](Draw& x){x.raster.src=Blend::SrcAlpha;});
  pipelineChange([](Draw& x){x.raster.dst=Blend::One;});
  pipelineChange([](Draw& x){x.raster.srcAlpha=Blend::DstColor;});
  pipelineChange([](Draw& x){x.raster.dstAlpha=Blend::SrcAlpha;});
  pipelineChange([](Draw& x){x.raster.blend=BlendOp::Max;});
  pipelineChange([](Draw& x){x.raster.blendAlpha=BlendOp::Min;});
  pipelineChange([](Draw& x){x.raster.colorMask=3;});
  pipelineChange([](Draw& x){x.attributes[1].slot=2;});
  pipelineChange([](Draw& x){x.attributes[1].stream=1;});
  pipelineChange([](Draw& x){x.attributes[1].offset=16;});
  pipelineChange([](Draw& x){x.attributes[1].type=Element::Color;});
  pipelineChange([](Draw& x){x.strides[0]=24;});
  pipelineChange([](Draw& x){x.perInstanceMask=1;});
  pipelineChange([](Draw& x){x.stepRates[0]=2;});
  pipelineChange([](Draw& x){x.attributes.pop_back();});
  auto depthChange=[&](auto change){Raster x;change(x);assert(depthStateKey(x)!=depth);};
  depthChange([](Raster& x){x.depthOn=false;});
  depthChange([](Raster& x){x.depthWrite=false;});
  depthChange([](Raster& x){x.depthCompare=Compare::Greater;});
  depthChange([](Raster& x){x.stencilOn=true;});
  depthChange([](Raster& x){x.stencilCompare=Compare::Less;});
  depthChange([](Raster& x){x.stencilFail=StencilOp::Replace;});
  depthChange([](Raster& x){x.stencilDepthFail=StencilOp::Zero;});
  depthChange([](Raster& x){x.stencilPass=StencilOp::Invert;});
  depthChange([](Raster& x){x.stencilRead=7;});
  depthChange([](Raster& x){x.stencilWrite=3;});
  depthChange([](Raster& x){x.twoSidedStencil=true;});
  { Raster a;a.twoSidedStencil=true;Raster b=a;b.backStencilCompare=Compare::Less;assert(depthStateKey(a)!=depthStateKey(b)); }
  { Raster a;a.twoSidedStencil=true;Raster b=a;b.backStencilFail=StencilOp::Replace;assert(depthStateKey(a)!=depthStateKey(b)); }
  { Raster a;a.twoSidedStencil=true;Raster b=a;b.backStencilDepthFail=StencilOp::Replace;assert(depthStateKey(a)!=depthStateKey(b)); }
  { Raster a;a.twoSidedStencil=true;Raster b=a;b.backStencilPass=StencilOp::Replace;assert(depthStateKey(a)!=depthStateKey(b)); }
  {Sampler a,b;b.minLinear=!a.minLinear;assert(samplerStateKey(a)!=samplerStateKey(b));}
  {Sampler a,b;b.magLinear=!a.magLinear;assert(samplerStateKey(a)!=samplerStateKey(b));}
  {Sampler a,b;b.mipLinear=!a.mipLinear;assert(samplerStateKey(a)!=samplerStateKey(b));}
  {Sampler a,b;b.mipEnabled=!a.mipEnabled;assert(samplerStateKey(a)!=samplerStateKey(b));}
  // Compare cache partitions with the previous key, including repeated layouts.
  std::mt19937 rng(17);std::unordered_map<std::string,std::string> forward,reverse;
  for(unsigned i=0;i<20000;++i) {
    auto n=rng()%64;auto&r=d.raster;
    r.blendOn=n&1;r.src=Blend(n%13);r.dst=Blend((n/3)%13);
    r.srcAlpha=Blend((n/5)%13);r.dstAlpha=Blend((n/7)%13);
    r.blend=BlendOp(n%5);r.blendAlpha=BlendOp((n/2)%5);r.colorMask=n%16;
    d.attributes[1].slot=1+n%3;d.attributes[1].stream=n%2;d.attributes[1].offset=n%16;
    d.attributes[1].type=Element(n%15);d.strides[1]=4*(n%8);
    d.perInstanceMask=n%4;d.stepRates[0]=n%5;d.stepRates[1]=n%7;
    auto a=oldPipeline(d,ps,80+n%2,252,n%3),b=pipelineStateKey(d,ps,80+n%2,252,n%3);
    auto f=forward.emplace(a,b),g=reverse.emplace(b,a);
    assert(f.first->second==b && g.first->second==a);
  }
  d.attributes[0].stream=16;bool rejected=false;try{key();}catch(const std::runtime_error&){rejected=true;}assert(rejected);
  d.attributes[0].stream=0;
  volatile size_t checksum=0;
  auto bench=[&](bool old){auto start=std::chrono::steady_clock::now();
    for(unsigned i=0;i<100000;++i){d.raster.colorMask=i%16;auto k=old?oldPipeline(d,ps,80,252,70):key();checksum+=std::hash<std::string>{}(k);}
    return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();};
  auto oldMs=bench(true),newMs=bench(false);
  std::printf("PASS: shader entries/stages, formats/MRT, vertex/instance layouts, depth/stencil, samplers; 20000 cache equivalence checks\n");
  std::printf("100000 pipeline key builds + hashes: formatted %.2f ms, binary %.2f ms (%.1fx); CPU microbenchmark only\n",oldMs,newMs,oldMs/newMs);
}
