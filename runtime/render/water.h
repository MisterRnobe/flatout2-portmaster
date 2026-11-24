#pragma once
#include "material.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
namespace fo2render {
inline constexpr MaterialHint worldWaterMaterial(){return {.04f,.08f,MaterialCue::WorldWater};}
// Verified against the user's pro_water.sha (shader-library entry 26 / ID 25)
// and its captured 164-byte ps.1.1 program. Never classify a generic 2D shader.
inline bool worldWaterProgram(uint64_t hash,size_t bytes,uint32_t activeCubes,bool premultiplied,
                              bool depthTest,bool depthWrite,bool alphaTest,bool stencil){
  return hash==0xf65220e267055268ull&&bytes==164&&(activeCubes&12)==12&&premultiplied&&
      depthTest&&!depthWrite&&!alphaTest&&!stencil;
}
inline bool waterViewBasis(const float* matrix,std::array<float,9>& basis){
  for(unsigned i=0;i<16;++i)if(!std::isfinite(matrix[i]))return false;
  for(unsigned row=0;row<3;++row){unsigned source=row==2?3:row;double length=0;
    for(unsigned k=0;k<3;++k)length+=double(matrix[source*4+k])*matrix[source*4+k];
    if(length<1e-12)return false;
    for(unsigned k=0;k<3;++k)basis[row*3+k]=float(matrix[source*4+k]/std::sqrt(length))*(row==2?-1:1);
  }
  return true;
}
inline std::string waterNormalSource(std::string source){
  // Save the decoded normal before the shader overwrites r1 with N.V.
  const auto normal=source.find("r0 = clamp(float4(dot(((v0.xyz - 0.5)");
  const auto ret=source.rfind("return output;");
  if(normal==std::string::npos||ret==std::string::npos||normal>=ret)
    throw std::runtime_error("WorldWater normal capture shader shape changed");
  source.insert(ret,"float3 fo2n=float3(dot(float3(fo2fx[48],fo2fx[49],fo2fx[50]),fo2wave),dot(float3(fo2fx[51],fo2fx[52],fo2fx[53]),fo2wave),dot(float3(fo2fx[54],fo2fx[55],fo2fx[56]),fo2wave));\nfloat fo2len=dot(fo2n,fo2n);\noutput.oC0=float4((isfinite(fo2len)&&fo2len>1e-12?fo2n*rsqrt(fo2len):float3(0,1,0))*.5+.5,1);\n");
  source.insert(normal,"float3 fo2wave=r1.xzy;\n");
  return source;
}
}
