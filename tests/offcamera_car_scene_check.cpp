#include "../runtime/guest/offcamera_car_scene.h"
#include "../runtime/render/ray_only_scope.h"
#include <cassert>
#include <cstdio>
#include <limits>
using namespace fo2offcamera;
static thread_local int nativeRayOnly=0;
extern "C" int fo2MetalSetRayOnly(int value){int old=nativeRayOnly;nativeRayOnly=value;return old;}
struct Fixture {
  std::vector<uint8_t> bytes=std::vector<uint8_t>(0x700000);
  static constexpr uint32_t context=0x80000,camera=0x81000,car=0x50000;
  void put(uint32_t a,uint32_t v){assert(a+4<=bytes.size());std::memcpy(bytes.data()+a,&v,4);}
  void matrix(uint32_t a,float x=0){float m[16]={1,0,0,0,0,1,0,0,0,0,1,0,x,0,0,1};std::memcpy(bytes.data()+a,m,64);}
  void node(uint32_t a,uint32_t next=0,float x=10){put(a,0x67ae30);put(a+0xc8,next);put(a+0x108,1);matrix(a+0x80,x);}
  bool read(uint32_t a,void* out,size_t n){if(a>bytes.size()||n>bytes.size()-a)return false;std::memcpy(out,bytes.data()+a,n);return true;}
  Selection select(){return fo2offcamera::select(context,camera,[&](uint32_t a,void* out,size_t n){return read(a,out,n);});}
  Fixture(){
    put(0x67ae34,0x54cd20);put(context+0x1bc,1);put(context+0x1ac,0x82000);put(context+0x1a8,0x82008);put(0x82000,0x61000);
    matrix(camera+0x80);put(0x696dc8,0x10000);put(0x10014,0x11000);put(0x10018,0x11004);put(0x11000,0x40000);put(0x4033c,car);put(car+0x338,0x60000);
    put(0x600c4,0x61000);node(0x61000,0x62000);node(0x62000,0x63000);put(0x62010,2);
    node(0x63000,0x64000,90);node(0x64000,0x65000);put(0x64118,1);node(0x65000);
  }
};
int main(){
  Fixture f;auto s=f.select();assert(s.cars==1&&s.visited==6&&s.visible==1&&s.hidden==1&&s.distant==1&&s.unsupported==1);
  assert(s.nodes.size()==1&&s.nodes[0].address==0x65000&&s.nodes[0].world==0x65080&&s.nodes[0].car==f.car);
  auto unchanged=f.bytes;s=f.select();assert(f.bytes==unchanged); // Selection never changes original lists/flags.
  f.matrix(0x65080,20);s=f.select();assert(s.nodes.size()==1);f.matrix(0x65080,81);assert(f.select().nodes.empty());
  f=Fixture();f.put(0x60010,2);assert(f.select().nodes.empty()); // Hidden ancestors remain hidden.
  f=Fixture();f.put(0x82000,0x60000);assert(f.select().nodes.empty()); // Visible ancestor may already submit descendants.
  f=Fixture();f.put(f.context+0x1b4,0x90000);f.put(0x90000,0x91000);f.put(0x90004,0x91010);f.put(0x90008,0x91020);f.put(0x91004,0x65080);
  auto queued=fo2offcamera::select(f.context,f.camera,[&](uint32_t a,void* out,size_t n){return f.read(a,out,n);},0x90000);assert(queued.nodes.empty());
  f.put(0x91004,0x66080);queued=fo2offcamera::select(f.context,f.camera,[&](uint32_t a,void* out,size_t n){return f.read(a,out,n);},0x90000);assert(queued.nodes.size()==1);
  f=Fixture();f.put(0x600c4,0xb0000);for(unsigned i=0;i<129;++i)f.node(0xb0000+i*0x200,i==128?0:0xb0000+(i+1)*0x200);
  assert(f.select().nodes.size()==128);
  f=Fixture();f.put(0x650c8,0x65000);bool rejected=false;try{f.select();}catch(...){rejected=true;}assert(rejected);
  f=Fixture();f.put(f.context+0x1bc,2);assert(f.select().nodes.empty());
  f=Fixture();f.put(0x308a0,1);assert(f.select().nodes.empty()); // Restricted presentation is not a full race population.
  for(unsigned bad=0;bad<6;++bad){f=Fixture();switch(bad){case 0:f.put(0x67ae34,1);break;case 1:f.put(f.context+0x1a8,0x82009);break;
    case 2:f.put(0x10018,0x11000+33*4);break;case 3:f.put(0x600c4,0xfffffff0);break;
    case 4:{float nan=std::numeric_limits<float>::quiet_NaN();std::memcpy(f.bytes.data()+0x65080,&nan,4);break;}
    case 5:f.put(f.camera+0x80+15*4,0);break;}
    rejected=false;try{f.select();}catch(...){rejected=true;}assert(rejected);}
  f=Fixture();rejected=false;try{fo2offcamera::select(f.context,0xffffffc0,[&](uint32_t a,void* out,size_t n){return f.read(a,out,n);});}catch(...){rejected=true;}assert(rejected);
  Provenance provenance;provenance.add(0x65080);provenance.setWorld(0x61080);provenance.enter();assert(!provenance.rayOnly());
  provenance.setWorld(0x65080);assert(provenance.rayOnly());provenance.leave();assert(!provenance.rayOnly()); // HUD cannot inherit the last hidden matrix.
  provenance.enter(false);assert(!provenance.rayOnly());provenance.leave(); // Other camera/shadow bucket is ordinary.
  provenance.enter();provenance.enter(false);assert(!provenance.rayOnly());provenance.leave();assert(provenance.rayOnly());provenance.leave();
  for(unsigned i=0;i<65;++i)provenance.enter();assert(!provenance.rayOnly());provenance.leave();assert(provenance.rayOnly());for(unsigned i=0;i<64;++i)provenance.leave();
  provenance.enter();provenance.enter();provenance.leave();assert(provenance.rayOnly());bool queuedTag=provenance.rayOnly();provenance.reset();assert(!provenance.rayOnly());
  {fo2render::RayOnlyScope command(queuedTag);assert(nativeRayOnly);{fo2render::RayOnlyScope nested(false);assert(!nativeRayOnly);}assert(nativeRayOnly);}assert(!nativeRayOnly);
  try{fo2render::RayOnlyScope command(queuedTag);throw std::runtime_error("test");}catch(...){}assert(!nativeRayOnly);
  std::puts("Offcamera scene: current hierarchy, visible/hidden/range/ABI/bounds/cycle rejection, read-only selection and queued world provenance passed");
}
