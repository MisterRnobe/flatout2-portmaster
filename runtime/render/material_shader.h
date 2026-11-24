#pragma once
#include <stdexcept>
#include <string>
namespace fo2render {
inline std::string materialOutputSource(std::string source,bool terrainMask=false){
  // For terrain, store coverage in alpha and premultiply F0 by that coverage.
  // RGB/alpha of the original game output remain untouched.
  const std::string values=terrainMask?
      "float4(fo2fx[24]*clamp(t0.a*t1.a,0.0,1.0),mix(0.9,fo2fx[25],clamp(t0.a*t1.a,0.0,1.0)),fo2fx[26],clamp(t0.a*t1.a,0.0,1.0))":
      "float4(fo2fx[24],fo2fx[25],fo2fx[26],1)";
  if(terrainMask && (source.find("r0 = t0 * t1;")==std::string::npos ||
      source.find("r0.xyz = clamp((v0.xyz * r0.www) + r0.xyz")==std::string::npos))
    throw std::runtime_error("Terrain specular mask shader shape changed");
  if(source.find("[[color(1)]]")!=std::string::npos)throw std::runtime_error("Material output conflicts with existing color attachment 1");
  if(source.find("struct fo2_fragment_Output")!=std::string::npos){
    auto brace=source.find('{',source.find("struct fo2_fragment_Output"));
    auto ret=source.rfind("return output;");
    if(brace==std::string::npos||ret==std::string::npos)throw std::runtime_error("Unsupported structured material shader output");
    source.insert(ret,"output.fo2material="+values+";\n");
    source.insert(brace+1,"\nfloat4 fo2material [[color(1)]];\n");
  }else{
    const std::string entry="fragment float4 fo2_fragment(";
    auto at=source.find(entry),ret=source.rfind("return cur;");
    if(at==std::string::npos||ret==std::string::npos)throw std::runtime_error("Unsupported fixed material shader output");
    source.replace(ret,11,"return Fo2MaterialOutput{cur,"+values+"};");
    source.replace(at,entry.size(),"struct Fo2MaterialOutput{float4 color [[color(0)]];float4 material [[color(1)]];};\nfragment Fo2MaterialOutput fo2_fragment(");
  }
  return source;
}
}
