#pragma once
#include "trace_mesh.h"
#include "shader_capture.h"
#include <regex>
namespace fo2render {
// First bounded hit-material family: the inspected, linear-position car body.
// No guessed skinning, material classification or previous-frame geometry.
inline constexpr uint64_t traceCarVS=2238934220545660713ull,traceCarPS=17901388520841627836ull;
struct TraceCarMaterial {
  std::shared_ptr<Shader> vs,ps;
  Bytes vsConstants,psConstants;
  // Four float4 vertex inputs in the normalizer's compact vertex order.
  std::vector<std::array<float,16>> inputs;
  std::array<std::shared_ptr<Image>,4> textures;
  std::array<uint64_t,4> versions{};
  std::array<Sampler,4> samplers;
  uint32_t srgbMask=0;
  std::array<float,64> effects{};
  bool current()const{
    for(unsigned i=0;i<4;++i)if(!textures[i]||textures[i]->version!=versions[i])return false;
    return true;
  }
};
inline std::shared_ptr<const TraceCarMaterial> prepareTraceCarMaterial(const Draw& d,const TraceMesh& mesh){
  if(!d.vs||!d.ps||d.vs->hash!=traceCarVS||d.ps->hash!=traceCarPS||d.ps->cubeSamplerMask!=10||
     d.raster.blendOn||d.effects[0]||d.instanceCount!=1||d.perInstanceMask||
     mesh.positions.size()>16384||d.vsConstants.size()!=224||d.psConstants.size()!=32)return {};
  auto m=std::make_shared<TraceCarMaterial>();
  for(unsigned i=0;i<4;++i){auto t=d.textures[i];
    if(!t||t->parent||t->depth||t==d.color||t==d.depth||t->faces!=(i%2?6u:1u)||!t->width||!t->height||
       t->width>4096||t->height>4096||!t->levels||t->levels>13||t->rgba.size()<size_t(t->levels)*t->faces)return {};
    for(unsigned f=0;f<t->faces;++f)if(t->rgba[f*t->levels].size()!=size_t(t->width)*t->height*4)return {};
    m->textures[i]=t;m->versions[i]=t->version;m->samplers[i]=d.samplers[i];
  }
  std::vector<uint32_t> sources;try{sources=traceSourceIndices(d,mesh.positions.size());}catch(const std::runtime_error&){return {};}
  m->inputs.resize(sources.size());
  for(unsigned slot=0;slot<4;++slot){const Attribute* attr=nullptr;
    for(const auto& a:d.attributes)if(a.slot==slot){if(attr)return {};attr=&a;}
    if(!attr||attr->stream>=16||!d.streams[attr->stream]||d.stepRates[attr->stream])return {};
    unsigned count=0;bool color=attr->type==Element::Color;
    switch(attr->type){case Element::Float1:count=1;break;case Element::Float2:count=2;break;
      case Element::Float3:count=3;break;case Element::Float4:count=4;break;case Element::Color:count=1;break;default:return {};}
    auto stride=d.strides[attr->stream];auto& bytes=d.streams[attr->stream]->bytes;size_t n=count*4;
    if(!stride||uint64_t(attr->offset)+n>stride)return {};
    for(size_t v=0;v<sources.size();++v){uint64_t at=uint64_t(d.offsets[attr->stream])+attr->offset+uint64_t(sources[v])*stride;
      if(at>bytes.size()||n>bytes.size()-at)return {};
      float value[4]={0,0,0,1};
      // Preserve Metal's original normalized UChar4 order; the translated VS
      // performs the D3DCOLOR .zyxw swizzle itself.
      if(color)for(unsigned k=0;k<4;++k)value[k]=bytes[at+k]/255.f;
      else std::memcpy(value,bytes.data()+at,n);
      for(unsigned k=0;k<4;++k){if(!std::isfinite(value[k]))return {};m->inputs[v][slot*4+k]=value[k];}
    }
  }
  m->vs=d.vs;m->ps=d.ps;m->vsConstants=d.vsConstants;m->psConstants=d.psConstants;
  m->effects=d.effects;m->effects[0]=m->effects[3]=0;m->effects[61]=d.rayOnly&&d.rayOnlyDetached?1.f:0.f;
  m->effects[62]=d.rayOnly?1.f:0.f;m->effects[63]=d.colorSrgb?1.f:0.f;m->srgbMask=d.textureSrgbMask;
  return m;
}
inline std::string traceCarMaterialSource(const Shader& vs,const Shader& ps){
  if(vs.hash!=traceCarVS||ps.hash!=traceCarPS||shaderHash(vs.source.data(),vs.source.size())!=traceCarVS||
     shaderHash(ps.source.data(),ps.source.size())!=traceCarPS)throw std::runtime_error("Unverified RT car program");
  auto v=vs.source,p=ps.source;
  auto effects=p.find("{float4 fo2color=output.oC0;");auto ret=p.rfind("return output;");
  if(effects==std::string::npos||ret==std::string::npos||effects>=ret)throw std::runtime_error("RT car pixel epilogue changed");
  p.erase(effects,ret-effects); // Primary-view fog/alpha-test epilogue is inapplicable here.
  for(unsigned i=0;i<4;++i){auto from="bias(fo2fx["+std::to_string(32+i)+"])";auto at=p.find(from);
    if(at==std::string::npos)throw std::runtime_error("RT car sample layout changed");p.replace(at,from.size(),"level(0.0)");}
  auto plain=[](std::string s,const std::string& stage){
    auto at=s.find(stage+" fo2_");if(at==std::string::npos)throw std::runtime_error("RT car entry missing");s.erase(at,stage.size()+1);
    s=std::regex_replace(s,std::regex(R"(\[\[[^\]]*\]\])"),"");
    for(const char* text:{"fo2_vertex_Output output;","fo2_fragment_Output output;"}){
      auto pos=s.find(text);if(pos!=std::string::npos)s.insert(pos+std::strlen(text)-1,"{}");}
    return s;
  };
  return plain(v,"vertex")+plain(p,"fragment")+R"MSL(
struct CarVaryings {float4 t0,t1,t2,t3,v0,v1;};
struct CarHit {float4 baryConfidence;uint4 identity;};
kernel void traceCarVertices(constant fo2_vertex_Uniforms& vu [[buffer(0)]],
 constant float* fx [[buffer(2)]],device const float4* inputs [[buffer(3)]],
 device CarVaryings* output [[buffer(4)]],uint v [[thread_position_in_grid]]){
 fo2_vertex_Input i={};i.v0=inputs[v*4];i.v1=inputs[v*4+1];i.v2=inputs[v*4+2];i.v3=inputs[v*4+3];
 auto o=fo2_vertex(vu,i,fx);output[v]={o.oT0,o.oT1,o.oT2,o.oT3,o.oD0,o.oD1};
}
kernel void traceCarShade(constant fo2_fragment_Uniforms& pu [[buffer(1)]],constant float* fx [[buffer(2)]],
 device const CarVaryings* varyings [[buffer(3)]],device const uint* indices [[buffer(4)]],
 device const CarHit* hits [[buffer(5)]],constant uint4& range [[buffer(6)]],device atomic_uint* counts [[buffer(7)]],
 texture2d<float> s0_texture [[texture(0)]],sampler s0 [[sampler(0)]],
 texturecube<float> s1_texture [[texture(1)]],sampler s1 [[sampler(1)]],
 texture2d<float> s2_texture [[texture(2)]],sampler s2 [[sampler(2)]],
 texturecube<float> s3_texture [[texture(3)]],sampler s3 [[sampler(3)]],
 texture2d<float,access::write> output [[texture(4)]],uint2 p [[thread_position_in_grid]]){
 uint2 size=uint2(output.get_width(),output.get_height());if(any(p>=size))return;
 auto h=hits[p.y*size.x+p.x];if(!h.identity.x)return;uint primitive=h.identity.x-1;
 if(primitive<range.x||primitive>=range.x+range.y||h.baryConfidence.z<=0)return;
 uint3 ix=uint3(indices[primitive*3],indices[primitive*3+1],indices[primitive*3+2])-range.z;
 if(any(ix>=range.w))return;
 auto a=varyings[ix.x],b=varyings[ix.y],c=varyings[ix.z];
 float3 w=float3(1-h.baryConfidence.x-h.baryConfidence.y,h.baryConfidence.xy);
 if(!all(isfinite(w))||any(w<-.001)||any(w>1.001))return;
 fo2_fragment_Input i={};
 i.t0=a.t0*w.x+b.t0*w.y+c.t0*w.z;i.t1=a.t1*w.x+b.t1*w.y+c.t1*w.z;
 i.t2=a.t2*w.x+b.t2*w.y+c.t2*w.z;i.t3=a.t3*w.x+b.t3*w.y+c.t3*w.z;
 i.v0=a.v0*w.x+b.v0*w.y+c.v0*w.z;i.v1=a.v1*w.x+b.v1*w.y+c.v1*w.z;
 auto shaded=fo2_fragment(s0_texture,s0,s1_texture,s1,s2_texture,s2,s3_texture,s3,pu,i,fx,float4(0,0,0,1));
 if(!all(isfinite(shaded.oC0)))return;
 float3 rgb=clamp(shaded.oC0.rgb,0.0,1.0);
 // Match the stored primary color's transfer function, as the compositor
 // reads its ordinary RGBA8 view rather than an sRGB decoding view.
 if(fx[63]>0)rgb=select(12.92*rgb,1.055*pow(rgb,float3(1.0/2.4))-.055,rgb>float3(.0031308));
 output.write(float4(rgb,h.baryConfidence.z),p);
 atomic_fetch_add_explicit(counts+5,1u,memory_order_relaxed);
 if(h.identity.y)atomic_fetch_add_explicit(counts+6,1u,memory_order_relaxed);
 if(h.identity.y==1)atomic_fetch_add_explicit(counts+7,1u,memory_order_relaxed);
 if(fx[62]>0)atomic_fetch_add_explicit(counts+8,1u,memory_order_relaxed);
 if(fx[61]>0)atomic_fetch_add_explicit(counts+9,1u,memory_order_relaxed);
}
)MSL";
}
}
