#include "../runtime/render/live_trace_scene.h"
#include <cassert>
#include <cstdio>
using namespace fo2render;
static Draw fixture(){
  Draw d;d.vs=std::make_shared<Shader>();d.vs->inputs={{0,0,0}};
  d.attributes={{0,0,0,Element::Float3}};d.strides[0]=12;d.count=3;
  d.streams[0]=std::make_shared<Buffer>();float v[]={-1,-1,-2,1,-1,-2,0,1,-2,2,1,-2};
  d.streams[0]->bytes.resize(sizeof v);std::memcpy(d.streams[0]->bytes.data(),v,sizeof v);
  d.indices=std::make_shared<Buffer>();uint32_t idx[]={0,1,2,1,2,3};d.indices->bytes.resize(sizeof idx);std::memcpy(d.indices->bytes.data(),idx,sizeof idx);d.indexSize=4;return d;
}
static void matches(const Draw& d,const CachedTraceMesh& cached){auto expected=prepareTraceMesh(d);assert(cached.status==expected.status);if(cached.mesh){assert(cached.mesh->positions==expected.mesh.positions&&cached.mesh->indices==expected.mesh.indices);}}
int main(){
  TraceMeshCache cache;auto d=fixture();auto first=cache.prepare(d,{});assert(!first.hit);matches(d,first);
  auto again=cache.prepare(d,{});assert(again.hit&&again.mesh==first.mesh);
  d.vsConstants={1,2,3,4};assert(cache.prepare(d,{}).hit); // Constants never key raw geometry.
  float changed=-3;std::memcpy(d.streams[0]->bytes.data(),&changed,4);++d.streams[0]->version;
  auto update=cache.prepare(d,{});assert(!update.hit&&update.mesh->positions[0][0]==-3);matches(d,update);assert(first.mesh->positions[0][0]==-1);
  uint32_t index=3;std::memcpy(d.indices->bytes.data(),&index,4);++d.indices->version;update=cache.prepare(d,{});assert(!update.hit);matches(d,update);
  d.indexStart=3;update=cache.prepare(d,{});assert(!update.hit);matches(d,update);
  d.baseVertex=-1;update=cache.prepare(d,{});assert(!update.hit);matches(d,update);
  d=fixture();d.indices.reset();cache.prepare(d,{});d.vertexStart=1;update=cache.prepare(d,{});assert(!update.hit);matches(d,update);
  d=fixture();cache.prepare(d,{});d.streams[0]->bytes.push_back(0);update=cache.prepare(d,{});assert(!update.hit);matches(d,update);
  d=fixture();cache.prepare(d,{});d.streams[0]->transient=true;std::memcpy(d.streams[0]->bytes.data(),&changed,4);update=cache.prepare(d,{});assert(!update.hit);matches(d,update);
  d=fixture();cache.prepare(d,{});d.attributes[0].type=Element::Float4;d.strides[0]=16;update=cache.prepare(d,{});assert(!update.hit);matches(d,update);
  d=fixture();cache.prepare(d,{});auto rejected=cache.prepare(d,{300000,2,100});assert(rejected.status==TraceMeshStatus::Limit);
  assert(cache.prepare(d,{2,100,100}).status==TraceMeshStatus::Limit);
  TraceMeshCache bounded(64,1);auto one=bounded.prepare(d,{});auto other=fixture();bounded.prepare(other,{});assert(bounded.bytes()<=64&&bounded.entries()==1);assert(!bounded.prepare(d,{}).hit);assert(one.mesh->indices.size()==3);
  std::weak_ptr<Buffer> weak=d.streams[0];d={};assert(weak.expired()); // Cache retains no game buffers.
  // Same object address and version, new allocation ownership.
  alignas(Buffer) unsigned char storage[sizeof(Buffer)];auto construct=[&]{return std::shared_ptr<Buffer>(new(storage) Buffer,[](Buffer* p){p->~Buffer();});};
  d=fixture();d.streams[0]=construct();d.streams[0]->bytes=fixture().streams[0]->bytes;
  cache.prepare(d,{});d.streams[0].reset();d.streams[0]=construct();d.streams[0]->bytes=fixture().streams[0]->bytes;
  std::memcpy(d.streams[0]->bytes.data(),&changed,4);update=cache.prepare(d,{});assert(!update.hit);matches(d,update);d={};
  cache.clear();assert(cache.bytes()==0&&cache.entries()==0);
  std::puts("PASS: immutable mesh reuse, vertex/index versions, layout/range/size/transient changes, weak identity at reused addresses, budgets and eviction");
}
