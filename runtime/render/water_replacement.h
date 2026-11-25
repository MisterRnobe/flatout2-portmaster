#pragma once
#include <cstdlib>
#include <regex>
#include <stdexcept>
#include <string>
namespace fo2render {
inline std::string waterLegacyAttachment(std::string source){
  auto out=source.find("struct fo2_fragment_Output"),ret=source.rfind("return output;");
  if(out==std::string::npos||ret==std::string::npos||source.find("[[color(1)]]")!=std::string::npos)
    throw std::runtime_error("Water legacy attachment shape changed");
  auto brace=source.find('{',out);if(brace==std::string::npos)throw std::runtime_error("Water output missing");
  source.insert(brace+1,"\nfloat4 fo2legacy [[color(1)]];\n");return source;
}
inline std::string waterLegacyZeroSource(std::string source){
  source=waterLegacyAttachment(std::move(source));
  source.insert(source.rfind("return output;"),"output.fo2legacy=float4(.5,.5,.5,0);\n");return source;
}
// Only the inspected opaque pro_window_static family: replace its authored
// cube-texture mix, preserving the later direct-light term, fog and alpha.
// The signed stored-color delta is packed into [0,1] with <=1/255 error.
inline std::string waterLegacyWindowSource(std::string source){
  const std::string mix="r0.xyz = mix(r0.xyz, t1.xyz, r0.www);";
  if(source.find("texturecube<float> s1_texture")==std::string::npos||source.find("r0.w = t0.w * v0.w;")==std::string::npos||
     source.find("r1.w = t0.w * t1.w;")==std::string::npos||source.find(mix)==std::string::npos)
    throw std::runtime_error("Unverified water cube-reflection family");
  source=waterLegacyAttachment(std::move(source));
  auto at=source.find("fragment fo2_fragment_Output fo2_fragment"),body=source.find(") {",at);
  if(at==std::string::npos||body==std::string::npos||source.find("fo2_fragment_Uniforms")!=std::string::npos)
    throw std::runtime_error("Water reflection entry signature changed");
  body+=3;auto signature=source.substr(at,body-at);auto helper=signature;
  helper.erase(0,9);helper.replace(helper.find("fo2_fragment ("),14,"fo2_water_evaluate (");
  helper=std::regex_replace(helper,std::regex(R"(\[\[[^\]]*\]\])"),"");
  helper.insert(helper.rfind(')'),", bool fo2remove");
  auto code=source.substr(body);auto pos=code.find(mix);
  code.replace(pos,mix.size(),"float fo2coverage=clamp(r0.w,0.0,1.0);\nif(fo2remove) r0.xyz *= 1.0-fo2coverage; else "+mix);
  const std::string fog="fo2color.rgb=mix(float3(fo2fx[8],fo2fx[9],fo2fx[10]),fo2color.rgb,clamp(factor,0.0,1.0));";
  auto fogAt=code.find(fog);if(fogAt==std::string::npos)throw std::runtime_error("Water fog epilogue changed");
  code.insert(fogAt+fog.size(),"fo2coverage*=clamp(factor,0.0,1.0);");
  code.insert(code.rfind("return output;"),"output.fo2legacy=float4(clamp(t0.a,0.0,1.0),0,0,fo2coverage);\n");
  std::string wrapper=signature+R"MSL(
 auto a=fo2_water_evaluate(s0_texture,s0,s1_texture,s1,input,fo2fx,fo2position,false);
 auto b=fo2_water_evaluate(s0_texture,s0,s1_texture,s1,input,fo2fx,fo2position,true);
 if(a.fo2legacy.x<=1.0/255.0)discard_fragment();
 float3 original=clamp(a.oC0.rgb,0.0,1.0),neutral=clamp(b.oC0.rgb,0.0,1.0);
 fo2_fragment_Output output;
 output.oC0=float4(float3(fo2fx[48],fo2fx[49],fo2fx[50])*.5+.5,a.fo2legacy.x);
 output.fo2legacy=float4((original-neutral)*.5+.5,a.fo2legacy.w);
 return output;
}
)MSL";
  return source.substr(0,at)+helper+code+wrapper;
}
inline bool waterReplacementEnabled(){const char* flag=std::getenv("FO2_WATER_REPLACE_REFLECTION");return flag&&std::string(flag)=="1";}
}
