#pragma once
#include "material.h"
#include <cstddef>
namespace fo2render {
// pro_lightmapped_spec.sha, shader-library entry 3 / shader ID 2.
// tex t0; tex t1; mul r0,t0,t1; mad_sat r0.rgb,v0,r0.a,r0.
// The alpha product is the authored specular mask, not diffuse brightness.
inline bool terrainSpecularProgram(uint64_t hash,size_t bytes) {
  return hash==0x40e19e2958b0afb1ull && bytes==60;
}
inline bool terrainSpecularPass(uint32_t samplers,uint32_t cubes,bool depth,bool write,
                                bool blend,bool alphaTest,bool stencil) {
  return (samplers&3)==3 && !(cubes&3) && depth && write && !blend && !alphaTest && !stencil;
}
inline constexpr MaterialHint terrainSpecularMaterial() {
  // Dielectric F0 preset; authored exponent 8 gives sqrt(2/(8+2)).
  return {.04f,.4472136f,MaterialCue::TerrainSpecular};
}
}
