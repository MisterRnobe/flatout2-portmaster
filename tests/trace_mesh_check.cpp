#include "../runtime/render/trace_mesh.h"
#include <cassert>
#include <cstdio>
#include <limits>
using namespace fo2render;
static Draw fixture(){
  Draw d;d.vs=std::make_shared<Shader>();d.vs->inputs={{2,0,0}};
  d.attributes={{2,1,4,Element::Float3}};d.strides[1]=16;d.offsets[1]=16;
  d.streams[1]=std::make_shared<Buffer>();
  const float vertices[][4]={{99,99,99,99},{7,0,0,0},{7,1,0,0},{7,0,1,0},{7,1,1,0}};
  d.streams[1]->bytes.resize(sizeof vertices);std::memcpy(d.streams[1]->bytes.data(),vertices,sizeof vertices);
  d.count=3;return d;
}
static void indices(Draw& d,std::initializer_list<uint32_t> values,unsigned size){
  d.indices=std::make_shared<Buffer>();d.indexSize=size;d.indexStart=2;
  d.indices->bytes.resize((values.size()+2)*size,0xff);size_t at=2*size;
  for(auto value:values){std::memcpy(d.indices->bytes.data()+at,&value,size);at+=size;}
  d.count=uint32_t(values.size());
}
int main(){
  auto d=fixture();auto mesh=prepareTraceMesh(d);
  assert(mesh.status==TraceMeshStatus::Ready && mesh.mesh.indices==std::vector<uint32_t>({0,1,2}));
  assert((mesh.mesh.minimum==std::array<float,3>({0,0,0}) && mesh.mesh.maximum==std::array<float,3>({1,1,0})));
  d.streams[1]->bytes.assign(d.streams[1]->bytes.size(),0);assert(mesh.mesh.positions[1][0]==1);
  for(unsigned size:{2u,4u}){
    d=fixture();indices(d,{1,2,3},size);d.baseVertex=-1;
    auto a=prepareTraceMesh(d);assert(a.status==TraceMeshStatus::Ready && a.mesh.positions==mesh.mesh.positions);
    d.baseVertex=-2;assert(prepareTraceMesh(d).status==TraceMeshStatus::Invalid);
    d=fixture();indices(d,{0,1,2,3,size==2?0xffffu:0xffffffffu,0,1,2},size);d.primitive=Primitive::TriangleStrip;
    a=prepareTraceMesh(d);assert(a.status==TraceMeshStatus::Ready && a.mesh.restarts==1);
    assert(a.mesh.indices==std::vector<uint32_t>({0,1,2,2,1,3,0,1,2}));
    d=fixture();indices(d,{0,1,2,2,3,3,0,1,2},size);d.primitive=Primitive::TriangleStrip;
    a=prepareTraceMesh(d);assert(a.status==TraceMeshStatus::Ready && a.mesh.degenerate==4);
    assert(a.mesh.indices==std::vector<uint32_t>({0,1,2,0,3,1,0,1,2}));
    d=fixture();indices(d,{0,1,99},size);assert(prepareTraceMesh(d).status==TraceMeshStatus::Invalid);
    d=fixture();indices(d,{0,1,2},size);d.indices->bytes.pop_back();assert(prepareTraceMesh(d).status==TraceMeshStatus::Invalid);
  }
  d=fixture();d.vertexStart=1;auto start=prepareTraceMesh(d);assert(start.status==TraceMeshStatus::Ready && start.mesh.positions[0][0]==1);
  d=fixture();d.count=4;assert(prepareTraceMesh(d).status==TraceMeshStatus::Invalid);
  d=fixture();indices(d,{0,0,1},2);auto empty=prepareTraceMesh(d);assert(empty.status==TraceMeshStatus::Empty&&empty.mesh.degenerate==1);
  d=fixture();float nan=std::numeric_limits<float>::quiet_NaN();std::memcpy(d.streams[1]->bytes.data()+20,&nan,4);
  assert(prepareTraceMesh(d).status==TraceMeshStatus::Invalid);
  d=fixture();d.instanceCount=2;assert(prepareTraceMesh(d).status==TraceMeshStatus::Unsupported);
  d=fixture();d.perInstanceMask=2;assert(prepareTraceMesh(d).status==TraceMeshStatus::Unsupported);
  d=fixture();d.attributes[0].type=Element::Half4;assert(prepareTraceMesh(d).status==TraceMeshStatus::Unsupported);
  d=fixture();d.attributes[0].stream=16;assert(prepareTraceMesh(d).status==TraceMeshStatus::Invalid);
  d=fixture();d.strides[1]=0;assert(prepareTraceMesh(d).status==TraceMeshStatus::Unsupported);
  d=fixture();d.primitive=Primitive::Lines;assert(prepareTraceMesh(d).status==TraceMeshStatus::Unsupported);
  d=fixture();d.vs->inputs[0][1]=9;assert(prepareTraceMesh(d).status==TraceMeshStatus::Unsupported);
  d=fixture();d.attributes.push_back(d.attributes.front());assert(prepareTraceMesh(d).status==TraceMeshStatus::Invalid);
  d=fixture();assert(prepareTraceMesh(d,{2,100,100}).status==TraceMeshStatus::Limit);
  assert(prepareTraceMesh(d,{100,2,100}).status==TraceMeshStatus::Limit);
  assert(prepareTraceMesh(d,{100,100,0}).status==TraceMeshStatus::Limit);
  d=fixture();d.indexSize=3;d.indices=std::make_shared<Buffer>();assert(prepareTraceMesh(d).status==TraceMeshStatus::Invalid);
  d=fixture();indices(d,{0xffffffff,0xffffffff,0xffffffff},4);d.baseVertex=INT32_MAX;d.strides[1]=UINT32_MAX;
  assert(prepareTraceMesh(d).status==TraceMeshStatus::Invalid);
  // POSITION Float4 is accepted only with homogeneous w=1.
  d=fixture();d.attributes[0].type=Element::Float4;d.attributes[0].offset=0;
  float v[][4]={{0,0,0,1},{1,0,0,1},{0,1,0,1}};d.offsets[1]=0;
  d.streams[1]->bytes.resize(sizeof v);std::memcpy(d.streams[1]->bytes.data(),v,sizeof v);
  assert(prepareTraceMesh(d).status==TraceMeshStatus::Ready);
  v[1][3]=2;std::memcpy(d.streams[1]->bytes.data(),v,sizeof v);assert(prepareTraceMesh(d).status==TraceMeshStatus::Unsupported);
  std::puts("PASS: owned POSITION mesh, 16/32-bit indices, offsets/base vertex, strip winding/restarts/connectors, finite bounds and bounded invalid input rejection");
}
