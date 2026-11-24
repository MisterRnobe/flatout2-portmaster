#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace fo2render {
enum class MaterialCue : uint32_t { Unknown=0, EnvironmentCube=1, FixedSpecular=2, WorldWater=3, TerrainSpecular=4 };
struct MaterialHint { float reflectivity=0,roughness=1;MaterialCue cue=MaterialCue::Unknown; };
inline MaterialHint classifyMaterial(uint32_t activeCubes,bool programmable,bool specularEnabled,
                                     float r,float g,float b,float power){
  // Programmable shaders do not have to use D3DMATERIAL9 or SPECULARENABLE.
  if(!programmable&&specularEnabled&&std::isfinite(r)&&std::isfinite(g)&&std::isfinite(b)&&std::isfinite(power)&&power>0){
    float strength=std::clamp(std::max({r,g,b}),0.f,1.f);
    if(strength>0)return {strength,std::clamp(std::sqrt(2.f/(power+2)),0.f,1.f),MaterialCue::FixedSpecular};
  }
  // A real sampled environment cube identifies a reflective shader family,
  // but it does not reveal its physical F0/roughness: use conservative presets.
  if(activeCubes)return {.3f,.25f,MaterialCue::EnvironmentCube};
  return {};
}
}
