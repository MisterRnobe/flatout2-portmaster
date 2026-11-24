#include "../runtime/render/debug_view.h"
#include <cassert>
#include <limits>
#include <cstdio>
using namespace fo2render;
int main(){
  float m[16]={2,0,0,0, 0,3,0,0, 0,0,1.1f,-.1f, 0,0,1,0};
  auto p=extractDebugProjection(m);
  assert(p.valid && std::abs(debugViewDistance(p,.5f)-1.f/6)<1e-6);
  // Rotate the object and translate it: z-translation must cancel from B.
  float transformed[16]={0,0,-4,8, 0,6,0,18, 2.2f,0,0,8.7f, 2,0,0,8};
  auto q=extractDebugProjection(transformed);
  assert(q.valid);
  for(int i=0;i<4;++i)assert(std::abs(q.values[i]-p.values[i])<1e-5);
  m[12]=m[13]=m[14]=0;assert(!extractDebugProjection(m).valid);
  m[14]=1;m[0]=std::numeric_limits<float>::infinity();assert(!extractDebugProjection(m).valid);
  m[0]=2;m[2]=1;assert(!extractDebugProjection(m).valid);
  assert(parseDebugView(nullptr)==DebugView::Off && parseDebugView("normals")==DebugView::Normals);
  assert(parseDebugView("ssr")==DebugView::SSR && parseDebugView("ssr-mask")==DebugView::SSRMask);
  assert(std::string(debugViewName(DebugView::SSR))=="ssr");
  bool rejected=false;try{parseDebugView("rt");}catch(...){rejected=true;}assert(rejected);
  std::puts("Projection checks passed: distance, transformed perspective, invalid inputs, explicit mode selection");
}
