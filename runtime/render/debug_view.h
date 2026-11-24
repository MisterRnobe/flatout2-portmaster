#pragma once
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
namespace fo2render {
enum class DebugView { Off, Depth, Normals, SSR, SSRMask };
inline DebugView parseDebugView(const char *name) {
  const std::string s=name?name:"off";
  if(s.empty()||s=="off")return DebugView::Off;
  if(s=="depth")return DebugView::Depth;
  if(s=="normals")return DebugView::Normals;
  if(s=="ssr")return DebugView::SSR;
  if(s=="ssr-mask")return DebugView::SSRMask;
  throw std::runtime_error("Unknown Metal debug view: "+s);
}
inline const char* debugViewName(DebugView mode){
  switch(mode){case DebugView::Depth:return "depth";case DebugView::Normals:return "normals";case DebugView::SSR:return "ssr";case DebugView::SSRMask:return "ssr-mask";default:return "off";}
}
struct DebugProjection {
  // clipZ = A * positiveViewDistance + B; RH view-space Z is -distance.
  std::array<float,4> values{}; // sx, sy, A, B
  bool valid=false;
};
inline DebugProjection extractDebugProjection(const float *m) {
  DebugProjection out;
  for(unsigned i=0;i<16;++i)if(!std::isfinite(m[i]))return out;
  auto dot=[&](unsigned a,unsigned b){double n=0;for(unsigned k=0;k<3;++k)n+=double(m[a*4+k])*m[b*4+k];return n;};
  const double w=dot(3,3),x=dot(0,0),y=dot(1,1);
  if(w<1e-12||x<1e-12||y<1e-12)return out;
  // Only a rigid/uniform-scale perspective transform is identifiable here.
  if(std::abs(dot(0,3))>1e-3*std::sqrt(x*w)||
     std::abs(dot(1,3))>1e-3*std::sqrt(y*w)||
     std::abs(dot(0,1))>1e-3*std::sqrt(x*y))return out;
  const double a=dot(2,3)/w,b=double(m[11])-a*m[15];
  if(a<.99999||a>10||b>=-1e-8||!std::isfinite(b))return out;
  out.values={float(std::sqrt(x/w)),float(std::sqrt(y/w)),float(a),float(b)};
  out.valid=true;return out;
}
inline float debugViewDistance(const DebugProjection& p,float depth) {
  return p.values[3]/(depth-p.values[2]);
}
} // namespace fo2render
