#include "../runtime/render/live_trace_scene.h"
#include <cassert>
#include <cstdio>
using namespace fo2render;
static Draw fixture(){
  Draw d;d.vs=std::make_shared<Shader>();d.ps=std::make_shared<Shader>();d.vs->hash=traceCarVS;d.ps->hash=traceCarPS;d.ps->cubeSamplerMask=10;
  d.vs->source="oPos.x = dot(v0, c0);oPos.y = dot(v0, c1);oPos.z = dot(v0, c2);oPos.w = dot(v0, c3);";
  d.vs->uniforms={{0,0,14}};d.vs->inputs={{0,0,0},{1,3,0},{2,10,0},{3,5,0}};
  d.vsConstants.resize(224);d.psConstants.resize(32);
  float rows[]={1,0,0,0,0,1,0,0,0,0,-1.01f,-.1f,0,0,-1,0};std::memcpy(d.vsConstants.data(),rows,sizeof rows);
  d.debugProjection={{1,1,1.01f,-.1f},true};d.color=std::make_shared<Image>();d.depth=std::make_shared<Image>();
  for(unsigned i=0;i<4;++i){auto t=std::make_shared<Image>();t->faces=i%2?6:1;t->rgba.resize(t->faces,Bytes{1,2,3,4});d.textures[i]=t;}
  struct Vertex {float p[3],n[3];uint8_t color[4];float uv[2];};
  Vertex vertices[]={{{-1,-1,-4},{0,1,0},{10,20,30,40},{.1f,.2f}},{{1,-1,-4},{0,1,0},{50,60,70,80},{.3f,.4f}},{{0,1,-4},{0,1,0},{90,100,110,120},{.5f,.6f}}};
  d.streams[0]=std::make_shared<Buffer>();d.streams[0]->bytes.resize(sizeof vertices+4);std::memcpy(d.streams[0]->bytes.data()+4,vertices,sizeof vertices);
  d.offsets[0]=4;d.strides[0]=sizeof(Vertex);d.attributes={{0,0,0,Element::Float3},{1,0,12,Element::Float3},{2,0,24,Element::Color},{3,0,28,Element::Float2}};
  uint16_t indices[]={3,1,2};d.indices=std::make_shared<Buffer>();d.indices->bytes.resize(sizeof indices);std::memcpy(d.indices->bytes.data(),indices,sizeof indices);
  d.baseVertex=-1;d.count=3;return d;
}
static std::shared_ptr<const TraceCarMaterial> material(const Draw& d){auto raw=prepareTraceMesh(d);assert(raw.status==TraceMeshStatus::Ready);return prepareTraceCarMaterial(d,raw.mesh);}
int main(){
  auto d=fixture();auto m=material(d);assert(m&&m->inputs.size()==3&&m->current());
  assert(m->inputs[0][0]==0&&m->inputs[0][1]==1&&m->inputs[0][12]==.5f);assert(m->inputs[0][3]==1);
  assert(m->inputs[0][8]==90/255.f&&m->inputs[0][10]==110/255.f); // Shader performs swizzle, not decoder.
  d.streams[0]->bytes.assign(d.streams[0]->bytes.size(),0);d.vsConstants.assign(224,0);assert(m->inputs[0][12]==.5f&&m->vsConstants!=d.vsConstants);
  ++d.textures[0]->version;assert(!m->current());
  d=fixture();d.colorSrgb=true;assert(material(d)->effects[63]==1);d.rayOnlyDetached=true;assert(material(d)->effects[61]==0);
  d.rayOnly=true;assert(material(d)->effects[62]==1&&material(d)->effects[61]==1);
  for(unsigned which=0;which<10;++which){d=fixture();switch(which){
    case 0:d.ps->hash=1;break;case 1:d.textures[1]->faces=1;break;case 2:d.textures[0]->rgba.clear();break;
    case 3:d.textures[0]->parent=std::make_shared<Image>();break;case 4:d.vsConstants.pop_back();break;
    case 5:d.effects[0]=1;break;case 6:d.raster.blendOn=true;break;case 7:d.ps->cubeSamplerMask=0;break;
    case 8:d.attributes[3].type=Element::Half2;break;case 9:{float nan=std::numeric_limits<float>::quiet_NaN();std::memcpy(d.streams[0]->bytes.data()+4+28,&nan,4);break;}}
    assert(!material(d));}
  d=fixture();auto saved=d.streams[0]->bytes;d.streams[0]->bytes.resize(saved.size()-4);assert(!material(d));
  d=fixture();d.attributes.push_back(d.attributes[3]);assert(!material(d));
  // UVs are owned afresh even when the raw POSITION mesh is a cache hit.
  d=fixture();d.streams[1]=std::make_shared<Buffer>();float uv[]={.1f,.2f,.3f,.4f,.5f,.6f};d.streams[1]->bytes.resize(sizeof uv);std::memcpy(d.streams[1]->bytes.data(),uv,sizeof uv);
  d.attributes[3]={3,1,0,Element::Float2};d.strides[1]=8;
  d.rayOnly=d.rayOnlyDetached=true;LiveTraceScene live(true,1,0,{},1,true,true);live.beginDisplayFrame(true,42);live.record(d);
  auto s=live.copyDisplayFrame(42,d.color,d.depth,d.debugProjection.values);assert(s&&s->parts.size()==1&&s->parts[0].car);
  float changed=.9f;std::memcpy(d.streams[1]->bytes.data()+16,&changed,4);++d.streams[1]->version;
  live.beginDisplayFrame(true,43);live.record(d);auto next=live.copyDisplayFrame(43,d.color,d.depth,d.debugProjection.values);
  assert(next&&next->cacheHits==1&&next->parts[0].car->inputs[0][12]==.9f&&s->parts[0].car->inputs[0][12]==.5f);
  finalizeLiveTraceSnapshot(*s);assert(s->cars.size()==1&&s->cars[0].firstTriangle==0&&s->cars[0].triangles==1&&s->cars[0].baseVertex==0&&s->cars[0].vertices==3);
  assert(s->rayOnlyDraws==1&&s->cars[0].material->effects[62]==1);
  assert(s->detachedDraws==1&&s->cars[0].material->effects[61]==1);
  assert(s->scene.indices.size()==3&&s->parts.empty());
  // Material caps do not remove the object's RT geometry.
  live.beginDisplayFrame(true,44);for(unsigned i=0;i<70;++i)live.record(d);
  auto many=live.copyDisplayFrame(44,d.color,d.depth,d.debugProjection.values);finalizeLiveTraceSnapshot(*many);assert(many->cars.size()==64&&many->scene.indices.size()==210);
  // A late independent part replaces an ordinary material, while all scene
  // triangles remain. A frozen receiver snapshot keeps its earlier ownership.
  d=fixture();live.beginDisplayFrame(true,46);
  for(unsigned i=0;i<64;++i)live.record(d);
  auto frozen=live.copyDisplayFrame(46,d.color,d.depth,d.debugProjection.values);
  d.rayOnly=true;live.record(d);d.rayOnlyDetached=true;live.record(d);
  auto priority=live.copyDisplayFrame(46,d.color,d.depth,d.debugProjection.values);
  finalizeLiveTraceSnapshot(*priority);finalizeLiveTraceSnapshot(*frozen);
  assert(priority->cars.size()==64&&priority->scene.indices.size()==66*3&&priority->carMaterialEvictions==2);
  unsigned offcamera=0,detached=0;for(auto& car:priority->cars){offcamera+=car.material->effects[62]>0;detached+=car.material->effects[61]>0;}
  assert(offcamera==2&&detached==1&&frozen->cars.size()==64);
  for(auto& car:frozen->cars)assert(car.material->effects[62]==0);
  // Invalid high-priority draws must not evict valid material; ordinary late
  // draws must not evict higher/equal-priority entries either.
  d.attributes.push_back(d.attributes[3]);live.record(d);
  d.attributes.pop_back();d.rayOnly=d.rayOnlyDetached=false;live.record(d);
  auto rejectedBudget=live.copyDisplayFrame(46,d.color,d.depth,d.debugProjection.values);finalizeLiveTraceSnapshot(*rejectedBudget);
  assert(rejectedBudget->cars.size()==64&&rejectedBudget->carMaterialEvictions==2&&rejectedBudget->carMaterialInvalid==1&&rejectedBudget->carMaterialBudgetDrops==1);
  // Target discard returns the captured materials' budget immediately.
  live.discardDisplayTarget(std::make_shared<Image>(),{}); // A different target has no effect.
  live.beginDisplayFrame(true,47);for(unsigned i=0;i<64;++i)live.record(d);
  live.discardDisplayTarget(d.color,d.depth);live.record(d);
  auto cleared=live.copyDisplayFrame(47,d.color,d.depth,d.debugProjection.values);finalizeLiveTraceSnapshot(*cleared);
  assert(cleared->cars.size()==1&&cleared->carMaterialBudgetDrops==0);
  // Exercise the byte budget independently of the 64-material count cap.
  d=fixture();auto triangle=d.streams[0]->bytes;unsigned stride=d.strides[0];
  d.indices.reset();d.baseVertex=0;d.count=16380;d.streams[0]->bytes.resize(4+size_t(d.count)*stride);
  for(unsigned i=0;i<d.count;++i)std::memcpy(d.streams[0]->bytes.data()+4+size_t(i)*stride,triangle.data()+4+(i%3)*stride,stride);
  ++d.streams[0]->version;live.beginDisplayFrame(true,48);
  for(unsigned i=0;i<9;++i)live.record(d);
  d.rayOnly=d.rayOnlyDetached=true;live.record(d);
  auto byteBudget=live.copyDisplayFrame(48,d.color,d.depth,d.debugProjection.values);finalizeLiveTraceSnapshot(*byteBudget);
  size_t retainedBytes=0;detached=0;for(auto& car:byteBudget->cars){retainedBytes+=car.material->inputs.size()*64;detached+=car.material->effects[61]>0;}
  assert(byteBudget->cars.size()==8&&retainedBytes<=8*1024*1024&&detached==1&&byteBudget->carMaterialBudgetDrops==1&&byteBudget->carMaterialEvictions==1);
  // The asynchronous diagnostic does not own material resources.
  LiveTraceScene diagnostic(true,1,0,{},1,true);diagnostic.beginDisplayFrame(true,45);diagnostic.record(d);
  auto plain=diagnostic.copyDisplayFrame(45,d.color,d.depth,d.debugProjection.values);finalizeLiveTraceSnapshot(*plain);assert(plain->cars.empty());
  bool rejected=false;try{traceCarMaterialSource(*d.vs,*d.ps);}catch(...){rejected=true;}assert(rejected); // Spoofed identity/source rejected.
  // Independent double-precision ray/triangle oracle for the GPU fixture.
  // Its blue material test must exercise actual offscreen intersections.
  TraceScene wall;wall.projection={1,1,1.01f,-.1f};wall.positions={{{-100,-100,-4}},{{100,-100,-4}},{{100,100,-4}},{{-100,100,-4}}};wall.indices={0,1,2,0,2,3};wall.meshIds={0,0};
  unsigned hits=0,offscreen=0;std::array<float,3> normal={230/255.f*2-1,128/255.f*2-1,204/255.f*2-1};
  float norm=0;for(auto v:normal)norm+=v*v;for(auto& v:normal)v/=std::sqrt(norm);
  for(unsigned y=8;y<16;++y)for(unsigned x=0;x<16;++x){
    std::array<float,3> p={((x*4+2.5f)/64*2-1-1.f/64)*2,(1-(y*4+2.5f)/64*2+1.f/64)*2,-2},n=normal;
    float dot=0,len=0;for(unsigned k=0;k<3;++k){dot+=n[k]*p[k];len+=p[k]*p[k];}if(dot>0)for(auto& v:n)v=-v;
    std::array<float,3> incident;dot=0;for(unsigned k=0;k<3;++k){incident[k]=p[k]/std::sqrt(len);dot+=incident[k]*n[k];}
    TraceRay ray;ray.mask={255,0,0,0};ray.originMin[3]=.25f;ray.directionMax[3]=60.25f;
    for(unsigned k=0;k<3;++k){ray.originMin[k]=p[k]+n[k]*.06f;ray.directionMax[k]=incident[k]-2*dot*n[k];}
    auto hit=traceSceneReference(wall,ray);if(!hit.valid)continue;++hits;
    float qx=ray.originMin[0]+ray.directionMax[0]*hit.distance,qy=ray.originMin[1]+ray.directionMax[1]*hit.distance;
    float u=(qx/4+1.f/64+1)/2,v=(1-(qy/4-1.f/64))/2;
    if(u<=.001||u>=.999||v<=.001||v>=.999)++offscreen;
  }assert(hits>20&&offscreen>20);
  std::puts("RT car material: source indices, attribute ownership, texture versions, bounds, snapshot ranges, cache independence and budgets passed");
}
