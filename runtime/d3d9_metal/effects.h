#pragma once
#include <string>
namespace fo2render {
// D3D9 applies these states after the programmable pixel shader.
inline std::string pixelEffects(const std::string &color,
                                const std::string &position,
                                const std::string &fog = "1.0",
                                bool fogAllowed = true) {
  std::string s = "{float4 fo2color=" + color + ";\n";
  if (fogAllowed)
    s += "if(fo2fx[3]>0){float distance=1.0/max(abs(" + position +
         ".w),1e-20);float factor=" + fog +
         ";int "
         "mode=int(fo2fx[4]);if(mode==1)factor=exp(-distance*fo2fx[7]);else "
         "if(mode==2)factor=exp(-pow(distance*fo2fx[7],2.0));else "
         "if(mode==3)factor=(fo2fx[6]-distance)/"
         "max(fo2fx[6]-fo2fx[5],1e-6);fo2color.rgb=mix(float3(fo2fx[8],fo2fx[9]"
         ",fo2fx[10]),fo2color.rgb,clamp(factor,0.0,1.0));}\n";
  s += "if(fo2fx[0]>0){float "
       "a=floor(clamp(fo2color.a,0.0,1.0)*255.0+0.5),b=floor(fo2fx[2]*255.0+0."
       "5);int f=int(fo2fx[1]);bool "
       "ok=f==8||(f==2&&a<b)||(f==3&&a==b)||(f==4&&a<=b)||(f==5&&a>b)||(f==6&&"
       "a!=b)||(f==7&&a>=b);if(!ok)discard_fragment();}\n" +
       color + "=fo2color;}\n";
  return s;
}
} // namespace fo2render
