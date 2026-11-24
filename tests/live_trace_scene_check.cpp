#include "../runtime/render/live_trace_scene.h"
#include <cassert>
#include <cstdio>
using namespace fo2render;
static Draw fixture(){
  Draw d;d.color=std::make_shared<Image>();d.depth=std::make_shared<Image>();
  d.vs=std::make_shared<Shader>();d.vs->hash=11799442587878920358ull;d.vs->inputs={{0,0,0}};
  d.vs->source="oPos.x = dot(v0, c0);oPos.y = dot(v0, c1);oPos.z = dot(v0, c2);oPos.w = dot(v0, c3);";
  // Non-square projection, camera looking along world +X, translated (10,20,30).
  float rows[16]={0,0,2,-60,0,3,0,-60,1.01f,0,0,-10.2f,1,0,0,-10};
  d.vs->uniforms={{0,0,4}};d.vsConstants.resize(sizeof rows);std::memcpy(d.vsConstants.data(),rows,sizeof rows);
  d.debugProjection={{2,3,1.01f,-.1f},true};
  float positions[9]={15,22,31,15,20,30,16,20,31};
  d.streams[0]=std::make_shared<Buffer>();d.streams[0]->bytes.resize(sizeof positions);std::memcpy(d.streams[0]->bytes.data(),positions,sizeof positions);
  d.attributes={{0,0,0,Element::Float3}};d.strides[0]=12;d.count=3;return d;
}
int main(){
  for(auto size:{std::array<uint32_t,2>{1280,800},{1920,1080},{16,16},{1,1}}){
    float x=1.f/size[0],y=-1.f/size[1];
    assert(traceRasterShiftMatches(x,y,size[0],size[1]));
    assert(traceRasterShiftMatches(0,0,size[0],size[1]));
    assert(!traceRasterShiftMatches(-x,y,size[0],size[1]));
    assert(!traceRasterShiftMatches(x,-y,size[0],size[1]));
    assert(!traceRasterShiftMatches(x*2,y,size[0],size[1]));
    // Independently project a view-space point, apply the actual shader's
    // clip += shift*w, then reconstruct with debugPosition's subtraction.
    double vx=1.25,vy=-.75,vz=-7,sx=2,sy=3,w=-vz;
    double clipX=sx*vx+x*w,clipY=sy*vy+y*w;
    assert(std::abs((clipX/w-x)*w/sx-vx)<1e-12);
    assert(std::abs((clipY/w-y)*w/sy-vy)<1e-12);
  }
  assert(!traceRasterShiftMatches(0,0,0,800));
  assert(!traceRasterShiftMatches(std::numeric_limits<float>::quiet_NaN(),0,1280,800));
  auto d=fixture();LiveTraceScene off;off.present(true,0);off.record(d);assert(!off.present(true,1));
  LiveTraceScene live(true,2,3);live.record(d);assert(!live.collecting());
  live.present(true,10);live.present(false,11);live.present(true,12);assert(!live.collecting());
  live.present(true,13);assert(live.collecting());live.record(d);
  // Mutating the original buffer/constants after record cannot change a sample.
  d.streams[0]->bytes.assign(d.streams[0]->bytes.size(),0);d.vsConstants.assign(d.vsConstants.size(),0);
  auto first=live.present(true,14);assert(first&&first->scene.positions.size()==3&&first->accepted==1);
  assert(first->frame==14&&first->sample==1);
  const auto p=first->scene.positions[0];assert(std::abs(p[0]-1)<1e-5f&&std::abs(p[1]-2)<1e-5f&&std::abs(p[2]+5)<1e-5f);
  d=fixture();float translated= -62;std::memcpy(d.vsConstants.data()+3*4,&translated,4);
  live.present(true,15);live.present(true,16);live.record(d);auto second=live.present(true,17);
  assert(second&&second->sample==2&&second->scene.positions.size()==3);assert(second->fingerprint!=first->fingerprint);
  assert(std::abs(second->scene.positions[0][0])<1e-5f&&first->scene.positions[0]==p);
  // Reset discards a partially recorded frame, rather than reusing old geometry.
  live.present(true,18);live.present(true,19);live.record(d);live.reset();assert(!live.collecting());
  live.present(true,20);live.present(true,21);d=fixture();d.vs->hash=1;live.record(d);
  auto third=live.present(true,22);assert(third&&third->unknown==1&&third->scene.indices.empty());assert(!live.enabled());
  LiveTraceScene budget(true,0,1,{128,400000,100000,100000});budget.present(true,0);d=fixture();
  for(unsigned i=0;i<130;++i)budget.record(d);
  auto limited=budget.present(true,1);assert(limited&&limited->accepted==128&&limited->limited==2&&limited->scene.positions.size()==384);
  // Largest render-target group wins; meshes from different targets never mix.
  LiveTraceScene targets(true,0,1);targets.present(true,0);d=fixture();targets.record(d);
  d.color=std::make_shared<Image>();targets.record(d);targets.record(d);
  auto selected=targets.present(true,1);assert(selected&&selected->scene.indices.size()==6);
  LiveTraceScene invalid(true,0,1);invalid.present(true,0);d=fixture();d.vsConstants.pop_back();invalid.record(d);
  d=fixture();d.vs->source+="oPos.x = dot(v0, c0);";invalid.record(d);
  d=fixture();d.debugProjection.values[0]=3;invalid.record(d);
  auto rejected=invalid.present(true,1);assert(rejected&&rejected->invalid==1&&rejected->unknown==1&&rejected->otherProjection==1&&rejected->scene.indices.empty());
  LiveTraceScene inputLimit(true,0,1,{128,400000,100000,100000});inputLimit.present(true,0);d=fixture();d.count=400001;inputLimit.record(d);inputLimit.record(fixture());
  auto capped=inputLimit.present(true,1);assert(capped&&capped->limited==2&&capped->scene.indices.empty());
  d=fixture();auto rows=tracePositionMatrix(d);auto original=d.vsConstants;
  d.vs->uniforms={{0,8,1},{1,0,1},{0,0,2},{0,2,2}};d.vsConstants.assign(96,0);
  std::memcpy(d.vsConstants.data()+16,original.data(),64);assert(tracePositionMatrix(d)==rows);
  d.vs->uniforms.push_back({0,0,1});bool caught=false;try{tracePositionMatrix(d);}catch(...){caught=true;}assert(caught);
  live.disable();assert(!live.enabled());
  LiveTraceScene wide(true,1,0,{},2);wide.present(true,0);assert(!wide.collecting());wide.present(true,1);assert(wide.collecting());
  d=fixture();for(unsigned i=0;i<600;++i)wide.record(d);
  auto broad=wide.present(true,2);assert(broad&&broad->accepted==600&&broad->limited==0&&broad->scene.meshIds.back()==599);assert(wide.enabled());
  wide.present(true,3,false);assert(!wide.collecting());wide.present(true,4,true);assert(wide.collecting());wide.record(fixture());
  auto fresh=wide.present(true,5);assert(fresh&&fresh->scene.positions.size()==3&&fresh->sample==2);
  for(unsigned i=0;i<130;++i){wide.present(true,6+i*2);wide.record(fixture());assert(wide.present(true,7+i*2));}
  wide.reset();wide.present(true,300);assert(!wide.collecting());wide.present(true,301);assert(wide.collecting());
  LiveTraceScene deferred(true,0,0,{},0,true);deferred.present(true,0);d=fixture();deferred.record(d);
  auto owned=deferred.present(true,1);assert(owned&&!owned->prepared&&owned->scene.positions.empty()&&owned->parts.size()==1);
  float z=33;std::memcpy(d.streams[0]->bytes.data()+8,&z,4);++d.streams[0]->version;
  float shift=-62;std::memcpy(d.vsConstants.data()+12,&shift,4);
  finalizeLiveTraceSnapshot(*owned);assert(owned->prepared&&owned->scene.positions[0]==p);
  deferred.present(true,2);deferred.record(d);auto updated=deferred.present(true,3);assert(updated&&updated->cacheMisses==1);
  finalizeLiveTraceSnapshot(*updated);assert(std::abs(updated->scene.positions[0][0]-2)<1e-5f);
  shift=-64;std::memcpy(d.vsConstants.data()+12,&shift,4);deferred.present(true,4);deferred.record(d);
  auto moved=deferred.present(true,5);assert(moved&&moved->cacheHits==1&&moved->cacheMisses==0);finalizeLiveTraceSnapshot(*moved);
  assert(std::abs(moved->scene.positions[0][0]-1)<1e-5f&&moved->fingerprint!=updated->fingerprint);
  deferred.reset();deferred.present(true,6);deferred.record(d);auto cold=deferred.present(true,7);assert(cold&&cold->cacheMisses==1&&cold->cacheHits==0);
  // Shading chooses its receiver target, not the diagnostic's largest group.
  LiveTraceScene display(true,1,0,{},1,true);d=fixture();auto receiver=d;
  display.beginDisplayFrame(true,400);display.record(receiver);
  d.color=std::make_shared<Image>();for(unsigned i=0;i<4;++i)display.record(d);
  auto exact=display.takeDisplayFrame(400,receiver.color,receiver.depth,receiver.debugProjection.values);
  assert(exact&&exact->frame==400&&exact->parts.size()==1);finalizeLiveTraceSnapshot(*exact);assert(exact->scene.positions[0]==p);
  assert(!display.takeDisplayFrame(400,receiver.color,receiver.depth,receiver.debugProjection.values));
  display.beginDisplayFrame(true,401);display.record(receiver);
  assert(!display.takeDisplayFrame(402,receiver.color,receiver.depth,receiver.debugProjection.values));
  display.beginDisplayFrame(true,403);display.record(receiver);display.beginDisplayFrame(true,404);
  assert(!display.takeDisplayFrame(404,receiver.color,receiver.depth,receiver.debugProjection.values));
  display.beginDisplayFrame(false,405);display.record(receiver);assert(!display.collecting());
  assert(!display.takeDisplayFrame(405,receiver.color,receiver.depth,receiver.debugProjection.values));
  display.beginDisplayFrame(true,406);display.record(receiver);auto wrong=receiver.debugProjection.values;wrong[0]*=2;
  assert(!display.takeDisplayFrame(406,receiver.color,receiver.depth,wrong));
  display.beginDisplayFrame(true,407);display.record(receiver);
  display.discardDisplayTarget({},receiver.depth);assert(!display.takeDisplayFrame(407,receiver.color,receiver.depth,receiver.debugProjection.values));
  display.beginDisplayFrame(true,408);display.record(receiver);display.discardDisplayTarget(d.color,{});
  auto retained=display.takeDisplayFrame(408,receiver.color,receiver.depth,receiver.debugProjection.values);assert(retained&&retained->cacheHits==1);
  display.beginDisplayFrame(true,409);display.record(receiver);display.reset();
  assert(!display.takeDisplayFrame(409,receiver.color,receiver.depth,receiver.debugProjection.values));
  display.beginDisplayFrame(true,410);receiver=fixture();display.record(receiver);
  auto frozen=display.copyDisplayFrame(410,receiver.color,receiver.depth,receiver.debugProjection.values);
  assert(frozen&&frozen->parts.size()==1&&display.collecting());
  display.discardDisplayTarget(receiver.color,receiver.depth);
  receiver.streams[0]->bytes.assign(receiver.streams[0]->bytes.size(),0);++receiver.streams[0]->version;
  display.beginDisplayFrame(true,411);finalizeLiveTraceSnapshot(*frozen);
  assert(frozen->frame==410&&frozen->scene.positions[0]==p);
  assert(!display.takeDisplayFrame(411,receiver.color,receiver.depth,receiver.debugProjection.values));
  std::puts("PASS: live RT transforms/cache, owned snapshots, budgets; display-frame/receiver/projection identity, single consumption, clears and pause/reset rejection");
}
