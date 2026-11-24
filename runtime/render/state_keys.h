#pragma once
#include "commands.h"
#include <cstring>
#include <stdexcept>
namespace fo2render {
// Process-local binary keys: explicit fields avoid padding, locale formatting and
// hash-only equality. Strings retain their length, including embedded zero bytes.
struct StateKeyWriter {
  std::string key;
  explicit StateKeyWriter(size_t capacity) { key.reserve(capacity); }
  void word(uint32_t value) { key.append(reinterpret_cast<const char*>(&value), sizeof(value)); }
  template<class T> void value(T v) { word(uint32_t(v)); }
  void real(float v) { uint32_t bits; std::memcpy(&bits,&v,sizeof(bits)); word(bits); }
  void shader(const Shader& s) {
    word(uint32_t(s.hash)); word(uint32_t(s.hash>>32)); value(s.vertex);
    word(uint32_t(s.entry.size())); key.append(s.entry);
  }
};
inline std::string samplerStateKey(const Sampler& s) {
  StateKeyWriter k(36);
  for(auto a:s.address) k.word(a);
  k.value(s.minLinear); k.value(s.magLinear); k.value(s.mipLinear); k.value(s.mipEnabled);
  k.real(s.maxAnisotropy); k.real(s.minLod);
  // lodBias is implemented in shader constants, not MTLSamplerDescriptor.
  return std::move(k.key);
}
inline std::string depthStateKey(const Raster& r) {
  StateKeyWriter k(60);
  k.value(r.depthOn); k.value(r.depthWrite); k.value(r.depthCompare);
  k.value(r.stencilOn); k.value(r.stencilCompare); k.value(r.stencilFail);
  k.value(r.stencilDepthFail); k.value(r.stencilPass); k.word(r.stencilRead);
  k.word(r.stencilWrite); k.value(r.twoSidedStencil);
  if(r.twoSidedStencil) {
    k.value(r.backStencilCompare); k.value(r.backStencilFail);
    k.value(r.backStencilDepthFail); k.value(r.backStencilPass);
  }
  return std::move(k.key);
}
inline std::string pipelineStateKey(const Draw& d,const Shader& ps,
                                    uint32_t color,uint32_t depth,uint32_t material) {
  StateKeyWriter k(80+d.vs->entry.size()+ps.entry.size()+28*d.attributes.size());
  k.shader(*d.vs); k.shader(ps);
  k.word(color); k.word(depth); k.value(d.raster.blendOn);
  k.value(d.raster.src); k.value(d.raster.dst); k.value(d.raster.srcAlpha);
  k.value(d.raster.dstAlpha); k.value(d.raster.blend); k.value(d.raster.blendAlpha);
  k.word(d.raster.colorMask); k.word(material);
  for(auto a:d.attributes) {
    if(a.stream>=d.streams.size() || a.slot>=31)
      throw std::runtime_error("Invalid Metal vertex attribute slot/stream");
    k.word(a.slot); k.word(a.stream); k.word(a.offset); k.value(a.type);
    k.word(d.strides[a.stream]); k.word((d.perInstanceMask>>a.stream)&1);
    k.word(d.stepRates[a.stream]);
  }
  return std::move(k.key);
}
inline std::string stateKeyHex(const std::string& key) {
  const char digits[]="0123456789abcdef";
  std::string out; out.reserve(key.size()*2);
  for(unsigned char c:key) { out+=digits[c>>4]; out+=digits[c&15]; }
  return out;
}
}
