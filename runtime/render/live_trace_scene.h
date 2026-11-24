#pragma once
#include "trace_mesh_cache.h"
#include "trace_scene.h"
#include "trace_car_material.h"
#include <optional>
#include <chrono>
namespace fo2render {
inline bool traceLinearProgram(const Shader& shader){
  if(shader.hash!=11799442587878920358ull&&shader.hash!=3727128660738880625ull&&shader.hash!=2238934220545660713ull)return false;
  for(unsigned k=0;k<4;++k){auto text=std::string("oPos.")+"xyzw"[k]+" = dot(v0, c"+std::to_string(k)+");";
    auto p=shader.source.find(text);if(p==std::string::npos||shader.source.find(text,p+text.size())!=std::string::npos)return false;}
  return true;
}
inline std::array<std::array<double,4>,4> tracePositionMatrix(const Draw& d){
  std::array<std::array<double,4>,4> rows{};std::array<bool,4> found{};size_t offset=0;
  for(auto u:d.vs->uniforms)if(u.kind==0){
    if(!u.count||u.index>=256||u.count>256-u.index||offset+size_t(u.count)*16>d.vsConstants.size())throw std::runtime_error("Invalid live RT packed constants");
    for(unsigned r=u.index;r<std::min(4u,u.index+u.count);++r){
      if(found[r])throw std::runtime_error("Duplicate live RT matrix constant");found[r]=true;
      float values[4];std::memcpy(values,d.vsConstants.data()+offset+(r-u.index)*16,16);
      for(unsigned k=0;k<4;++k){if(!std::isfinite(values[k]))throw std::runtime_error("Nonfinite live RT matrix");rows[r][k]=values[k];}
    }offset+=size_t(u.count)*16;
  }
  for(bool v:found)if(!v)throw std::runtime_error("Missing live RT position matrix");return rows;
}
inline std::array<float,4> traceMatrixProjection(const std::array<std::array<double,4>,4>& rows){
  auto dot=[&](unsigned a,unsigned b){double value=0;for(unsigned k=0;k<3;++k)value+=rows[a][k]*rows[b][k];return value;};
  double x=dot(0,0),y=dot(1,1),w=dot(3,3);
  if(std::min({x,y,w})<1e-12)throw std::runtime_error("Singular live RT projection");
  for(auto pair:{std::array<unsigned,2>{0,3},{1,3},{0,1}})
    if(std::abs(dot(pair[0],pair[1]))>1e-3*std::sqrt(dot(pair[0],pair[0])*dot(pair[1],pair[1])))throw std::runtime_error("Unsupported live RT projection");
  double a=dot(2,3)/w,b=rows[2][3]-a*rows[3][3];
  if(a<.99999||a>10||b>=-1e-8)throw std::runtime_error("Invalid live RT depth projection");
  return {float(std::sqrt(x/w)),float(std::sqrt(y/w)),float(a),float(b)};
}
inline bool traceProjectionMatches(const std::array<float,4>& a,const std::array<float,4>& b){
  for(unsigned k=0;k<4;++k)if(!std::isfinite(a[k])||!std::isfinite(b[k])||std::abs(a[k]-b[k])>std::max(2e-5f,2e-4f*std::max(std::abs(a[k]),std::abs(b[k]))))return false;return true;
}
// The position shader applies the D3D9 raster-center correction after the
// captured position matrix. Depth reconstruction subtracts this same NDC
// shift, so geometry must stay in the unshifted view coordinate system.
inline bool traceRasterShiftMatches(float x,float y,uint32_t width,uint32_t height){
  if(!width||!height||!std::isfinite(x)||!std::isfinite(y))return false;
  if(x==0&&y==0)return true; // Native synthetic fixtures have no D3D9 bias.
  float expectedX=1.f/width,expectedY=-1.f/height;
  return std::abs(x-expectedX)<=8*std::numeric_limits<float>::epsilon()*expectedX&&
         std::abs(y-expectedY)<=8*std::numeric_limits<float>::epsilon()*-expectedY;
}
struct LiveTracePart {
  std::shared_ptr<const TraceMesh> mesh;
  std::array<std::array<double,4>,4> rows;
  std::array<float,4> projection;uint32_t meshId=0;
  std::shared_ptr<const TraceCarMaterial> car;
  bool rayOnly=false;
  bool detached=false;
};
struct LiveTraceCarRange {std::shared_ptr<const TraceCarMaterial> material;uint32_t firstTriangle=0,triangles=0,baseVertex=0,vertices=0;};
struct LiveTraceSnapshot {
  TraceScene scene;uint64_t frame=0,fingerprint=0;
  uint64_t collectNs=0,prepareNs=0;
  uint32_t cacheHits=0,cacheMisses=0;size_t cacheBytes=0,cacheEntries=0;
  bool prepared=false;std::vector<LiveTracePart> parts;
  std::vector<LiveTraceCarRange> cars;
  uint32_t sample=0,attempts=0,accepted=0,unknown=0,invalid=0,limited=0,otherProjection=0,otherTarget=0,rayOnlyDraws=0,detachedDraws=0;
  uint32_t carMaterialBudgetDrops=0,carMaterialEvictions=0,carMaterialInvalid=0;
};
// The diagnostic worker uses immutable owned meshes and copied matrices only.
// Optional car materials belong to the synchronous shading collector: owned
// vertex inputs/constants plus version-gated texture references, never buffers.
inline void finalizeLiveTraceSnapshot(LiveTraceSnapshot& snapshot){
  if(snapshot.prepared)return;auto start=std::chrono::steady_clock::now();auto& scene=snapshot.scene;
  snapshot.rayOnlyDraws=snapshot.detachedDraws=0;
  size_t vertices=0,indices=0;for(const auto& part:snapshot.parts){vertices+=part.mesh->positions.size();indices+=part.mesh->indices.size();}
  if(vertices>traceSceneMaxVertices||indices>size_t(traceSceneMaxTriangles)*3)throw std::runtime_error("Deferred RT scene exceeds limits");
  scene.positions.reserve(vertices);scene.indices.reserve(indices);scene.meshIds.reserve(indices/3);
  for(const auto& part:snapshot.parts){const auto& mesh=*part.mesh;auto base=uint32_t(scene.positions.size());
    try{
      if(!traceProjectionMatches(scene.projection,part.projection))throw std::runtime_error("Deferred RT projection mismatch");
      for(const auto& p:mesh.positions){double clip[4]{};
        for(unsigned r=0;r<4;++r){clip[r]=part.rows[r][3];for(unsigned k=0;k<3;++k)clip[r]+=part.rows[r][k]*p[k];}
        auto projection=part.projection;
        std::array<float,3> view={float(clip[0]/projection[0]),float(clip[1]/projection[1]),float(-clip[3])};
        const double reproject[4]={double(view[0])*projection[0],double(view[1])*projection[1],-double(view[2])*projection[2]+projection[3],-double(view[2])};
        for(float v:view)if(!std::isfinite(v)||std::abs(v)>1e7f)throw std::runtime_error("Invalid live RT transformed vertex");
        for(unsigned k=0;k<4;++k)if(std::abs(clip[k]-reproject[k])>2e-4*std::max({1.,std::abs(clip[k]),std::abs(reproject[k])}))throw std::runtime_error("Live RT vertex reprojection mismatch");
        scene.positions.push_back(view);
      }
    }catch(const std::runtime_error&){scene.positions.resize(base);++snapshot.invalid;if(snapshot.accepted)--snapshot.accepted;continue;}
    if(part.car)snapshot.cars.push_back({part.car,uint32_t(scene.indices.size()/3),uint32_t(mesh.indices.size()/3),base,uint32_t(mesh.positions.size())});
    if(part.rayOnly)++snapshot.rayOnlyDraws;
    if(part.detached)++snapshot.detachedDraws;
    for(auto i:mesh.indices)scene.indices.push_back(base+i);
    scene.meshIds.insert(scene.meshIds.end(),mesh.indices.size()/3,part.meshId);
  }
  snapshot.parts.clear();if(!scene.indices.empty())validateTraceScene(scene);
  uint64_t hash=14695981039346656037ull;
  auto add=[&](const void* ptr,size_t size){auto p=static_cast<const uint8_t*>(ptr);for(size_t i=0;i<size;++i){hash^=p[i];hash*=1099511628211ull;}};
  add(scene.positions.data(),scene.positions.size()*12);add(scene.indices.data(),scene.indices.size()*4);
  add(scene.meshIds.data(),scene.meshIds.size()*4);add(scene.projection.data(),16);snapshot.fingerprint=hash;snapshot.prepared=true;
  snapshot.prepareNs=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
}
struct LiveTraceLimits {uint32_t draws=traceSceneMaxDraws,inputElements=1500000,vertices=traceSceneMaxVertices,triangles=traceSceneMaxTriangles;};
// Each frame starts empty. Immutable raw meshes and copied draw matrices may
// be merged/transformed later by the worker; no game buffers cross threads.
class LiveTraceScene {
  struct Group {std::shared_ptr<Image> color,depth;TraceScene scene;std::vector<LiveTracePart> parts;size_t triangles=0;};
  bool on=false,armed=false;uint32_t warmup=0,completed=0,interval=120,sampleLimit=3;
  uint32_t initialWarmup=120;bool warming=true;LiveTraceLimits limits;
  uint32_t inputWork=0,vertexWork=0,triangleWork=0;
  bool deferPreparation=false;TraceMeshCache cache;
  bool collectCarMaterials=false;size_t carBytes=0;uint32_t carDraws=0;
  LiveTraceSnapshot stats;std::vector<Group> groups;
  uint64_t displayFrame=0;bool displayArmed=false;
  void clear(){displayArmed=false;armed=false;inputWork=vertexWork=triangleWork=0;carBytes=0;carDraws=0;stats={};groups.clear();}
  std::shared_ptr<const TraceCarMaterial> retainCarMaterial(const Draw& d,const TraceMesh& mesh,
      const std::array<std::array<double,4>,4>& rows,const std::array<float,4>& projection){
    if(!collectCarMaterials||!traceCarDrawEligible(d))return {};
    if(mesh.positions.size()>16384){++stats.carMaterialInvalid;return {};}
    constexpr size_t byteLimit=8*1024*1024;size_t bytes=mesh.positions.size()*64;
    double clip[4]{};for(unsigned r=0;r<4;++r){clip[r]=rows[r][3];for(unsigned k=0;k<3;++k)clip[r]+=rows[r][k]*(double(mesh.minimum[k])+mesh.maximum[k])*.5;}
    float distance=float(std::sqrt(std::pow(clip[0]/projection[0],2)+std::pow(clip[1]/projection[1],2)+clip[3]*clip[3]));
    if(!std::isfinite(distance)){++stats.carMaterialInvalid;return {};}
    bool nearVisible=!d.rayOnly&&distance<=8;
    unsigned priority=traceCarPriority(d.rayOnly,d.rayOnlyDetached,nearVisible);
    std::vector<LiveTracePart*> candidates,evictions;
    size_t remainingBytes=carBytes;uint32_t remainingDraws=carDraws;
    if(remainingDraws>=64||bytes>byteLimit-remainingBytes){
      for(auto& group:groups)for(auto& part:group.parts)if(part.car){
        unsigned old=traceCarPriority(part.rayOnly,part.detached,part.car->effects[60]>0);
        if(old<priority||(priority==3&&old==3&&distance+.05f<part.car->cameraDistance))candidates.push_back(&part);
      }
      std::stable_sort(candidates.begin(),candidates.end(),[](auto a,auto b){
        auto pa=traceCarPriority(a->rayOnly,a->detached,a->car->effects[60]>0),pb=traceCarPriority(b->rayOnly,b->detached,b->car->effects[60]>0);
        return pa!=pb?pa<pb:pa==3&&a->car->cameraDistance>b->car->cameraDistance;});
      for(auto part:candidates){
        if(remainingDraws<64&&bytes<=byteLimit-remainingBytes)break;
        remainingBytes-=part->car->inputs.size()*64;--remainingDraws;evictions.push_back(part);
      }
      if(remainingDraws>=64||bytes>byteLimit-remainingBytes){++stats.carMaterialBudgetDrops;return {};}
    }
    // Validate and copy before eviction: a malformed late draw must not remove
    // an earlier material. Geometry is retained regardless of material priority.
    auto material=prepareTraceCarMaterial(d,mesh,distance);if(!material){++stats.carMaterialInvalid;return {};}
    for(auto part:evictions)part->car.reset();
    stats.carMaterialEvictions+=uint32_t(evictions.size());
    carBytes=remainingBytes+bytes;carDraws=remainingDraws+1;return material;
  }
public:
  explicit LiveTraceScene(bool enabled=false,uint32_t frames=120,uint32_t samples=3,LiveTraceLimits budget={},uint32_t initial=0,bool deferred=false,bool carMaterials=false):on(enabled),interval(frames),sampleLimit(samples),initialWarmup(initial?initial:frames),limits(budget),deferPreparation(deferred),collectCarMaterials(carMaterials){}
  bool enabled()const{return on&&(!sampleLimit||completed<sampleLimit);}
  bool collecting()const{return enabled()&&armed;}
  void disable(){on=false;warmup=0;clear();cache.clear();}
  void reset(){warmup=0;warming=true;clear();cache.clear();}
  // Synchronous shading has a different contract from the periodic worker:
  // collect one display frame, then consume only the exact receiver target.
  void beginDisplayFrame(bool racing,uint64_t frame){
    clear();displayFrame=frame;displayArmed=racing&&enabled();armed=displayArmed;
  }
  void discardDisplayTarget(const std::shared_ptr<Image>& color,const std::shared_ptr<Image>& depth){
    if(!displayArmed)return;
    for(auto& g:groups)if((color&&g.color==color)||(depth&&g.depth==depth))for(auto& part:g.parts)if(part.car){
      carBytes-=part.car->inputs.size()*64;--carDraws;
    }
    groups.erase(std::remove_if(groups.begin(),groups.end(),[&](const auto& g){return (color&&g.color==color)||(depth&&g.depth==depth);}),groups.end());
  }
  // Freeze at the same boundary as the receiver depth/color copy. Later
  // clears and buffer uploads cannot alter the saved immutable mesh handles.
  std::optional<LiveTraceSnapshot> copyDisplayFrame(uint64_t frame,const std::shared_ptr<Image>& color,
      const std::shared_ptr<Image>& depth,const std::array<float,4>& projection)const{
    if(!displayArmed||frame!=displayFrame||!color||!depth)return {};
    auto group=std::find_if(groups.begin(),groups.end(),[&](const auto& g){return g.color==color&&g.depth==depth&&traceProjectionMatches(g.scene.projection,projection);});
    if(group==groups.end())return {};
    auto result=stats;result.scene=group->scene;result.parts=group->parts;result.frame=frame;result.sample=completed+1;
    result.cacheBytes=cache.bytes();result.cacheEntries=cache.entries();
    if(!deferPreparation)finalizeLiveTraceSnapshot(result);return result;
  }
  std::optional<LiveTraceSnapshot> takeDisplayFrame(uint64_t frame,const std::shared_ptr<Image>& color,
      const std::shared_ptr<Image>& depth,const std::array<float,4>& projection){
    auto result=copyDisplayFrame(frame,color,depth,projection);if(result)++completed;clear();return result;
  }
  void record(const Draw& d){
    if(!collecting())return;
    struct Timer {uint64_t& total;std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();~Timer(){total+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();}} timer{stats.collectNs};
    if(stats.attempts>=limits.draws||inputWork>=limits.inputElements){++stats.limited;return;}
    ++stats.attempts;
    if(!d.vs||!traceLinearProgram(*d.vs)){++stats.unknown;return;}
    if(!d.debugProjection.valid){++stats.invalid;return;}
    try{
      auto rows=tracePositionMatrix(d);auto projection=traceMatrixProjection(rows);
      if(!traceProjectionMatches(projection,d.debugProjection.values)){++stats.otherProjection;return;}
      size_t group=0;while(group<groups.size()&&(groups[group].color!=d.color||groups[group].depth!=d.depth))++group;
      if(group==groups.size()){
        if(groups.size()>=4){++stats.otherTarget;return;}
        groups.push_back({d.color,d.depth,{}});groups.back().scene.projection=projection;
      }else if(!traceProjectionMatches(projection,groups[group].scene.projection)){++stats.otherProjection;return;}
      auto raw=cache.prepare(d,{limits.inputElements-inputWork,limits.vertices-vertexWork,limits.triangles-triangleWork});
      inputWork+=std::min(d.count,limits.inputElements-inputWork);
      if(raw.status!=TraceMeshStatus::Ready){if(raw.status==TraceMeshStatus::Limit)++stats.limited;else ++stats.invalid;return;}
      const auto& mesh=*raw.mesh;if(raw.hit)++stats.cacheHits;else ++stats.cacheMisses;
      auto car=retainCarMaterial(d,mesh,rows,projection);
      groups[group].parts.push_back({raw.mesh,rows,projection,stats.accepted,car,d.rayOnly,d.rayOnly&&d.rayOnlyDetached});++stats.accepted;groups[group].triangles+=mesh.indices.size()/3;
      if(d.rayOnly)++stats.rayOnlyDraws;
      vertexWork+=uint32_t(mesh.positions.size());triangleWork+=uint32_t(mesh.indices.size()/3);
    }catch(const std::exception&){++stats.invalid;}
  }
  std::optional<LiveTraceSnapshot> present(bool racing,uint64_t frame,bool allowArm=true){
    if(!enabled())return {};
    if(!racing){reset();return {};}
    if(armed){
      auto start=std::chrono::steady_clock::now();
      auto largest=std::max_element(groups.begin(),groups.end(),[](const auto& a,const auto& b){return a.triangles<b.triangles;});
      if(largest!=groups.end()){stats.scene=std::move(largest->scene);stats.parts=std::move(largest->parts);}
      stats.sample=++completed;stats.frame=frame;
      stats.cacheBytes=cache.bytes();stats.cacheEntries=cache.entries();
      if(!deferPreparation)finalizeLiveTraceSnapshot(stats);
      stats.collectNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
      auto result=std::move(stats);warmup=0;warming=false;clear();return result;
    }
    auto target=warming?initialWarmup:interval;
    if(warmup<target)++warmup;
    if(warmup>=target&&allowArm)armed=true;
    return {};
  }
};
}
