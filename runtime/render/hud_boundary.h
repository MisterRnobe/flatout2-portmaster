#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
namespace fo2render {
// Captured PosColorTex1 programs from pro_default_static.sha and its
// color/UV variants. fe37... is the equivalent synthetic ABI fixture.
inline bool knownHudVertex(uint64_t hash,size_t bytes){
  return (bytes==84&&(hash==0xe539ac0c9672e9baull||hash==0xfe37aa54ab7706feull))||
    (bytes==88&&(hash==0x119ca7f6d682b2aaull||hash==0x891cfe0449e3e459ull))||
    (bytes==96&&hash==0xa98a38246889657eull);
}
// Orthographic use + texture*vertex-color PS identifies a possible HUD boundary.
// The game keeps depth testing enabled for native orthographic HUD draws.
// Texture render targets/full-size postprocess inputs are deliberately excluded.
inline bool knownHudBoundary(uint64_t vs,size_t vsBytes,uint64_t ps,size_t psBytes,
                             const float* wRow,bool /*depth*/,bool alphaBlend,bool sampledTarget,
                             bool ordinaryTexture){
  if(!knownHudVertex(vs,vsBytes)||ps!=0x12ac2783e5b09938ull||psBytes!=32||
     !alphaBlend||sampledTarget||!ordinaryTexture)return false;
  for(unsigned i=0;i<4;++i)if(!std::isfinite(wRow[i]))return false;
  return std::abs(wRow[0])+std::abs(wRow[1])+std::abs(wRow[2])<1e-6f&&std::abs(wRow[3]-1)<1e-6f;
}
}
