#pragma once
#include "../render/physical_water.h"
#include "../render/shader_capture.h"
#import <Foundation/Foundation.h>
namespace fo2render {
class PhysicalWaterBridge {
  bool on=false;uint32_t asset=UINT32_MAX;
  PhysicalWaterRegion region;TraceMeshCache meshes{8*1024*1024,256};
  struct Match {std::weak_ptr<const TraceMesh> mesh;std::shared_ptr<Buffer> indices;};
  size_t cachedBytes=0;
  std::unordered_map<std::string,Match> results;
  uint64_t attempts=0,accepted=0,rejected=0,selectedTriangles=0;unsigned errors=0;
  std::array<uint64_t,3> familyAttempts{},familyAccepted{};
  void load(uint32_t next){
    asset=next;region.set({});meshes.clear();results.clear();cachedBytes=0;if(!next)return;
    const char* catalog=std::getenv("FO2_WATER_PHYSICS_SIGNATURES");if(!catalog||!*catalog)return;
    auto path=std::filesystem::path(catalog).parent_path()/("collision-"+std::to_string(next-1)+".json");
    if(std::filesystem::file_size(path)>64*1024*1024)throw std::runtime_error("Physical water JSON size limit");
    auto data=[NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path.c_str()]];if(!data)throw std::runtime_error("Physical water catalog missing");NSError* error=nil;
    id doc=[NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
    if(![doc isKindOfClass:[NSDictionary class]]||![doc[@"asset_index"] isKindOfClass:[NSNumber class]]||[doc[@"asset_index"] unsignedIntValue]!=next-1)
      throw std::runtime_error("Physical water catalog identity invalid");
    id list=doc[@"water_triangles"];
    if(![list isKindOfClass:[NSArray class]]||[list count]>250000)throw std::runtime_error("Physical water catalog triangles invalid");
    std::vector<PhysicalWaterRegion::Triangle> triangles;
    for(id item in list){
      if(![item isKindOfClass:[NSDictionary class]])throw std::runtime_error("Physical water triangle type invalid");
      id points=item[@"points"];
      if(![points isKindOfClass:[NSArray class]]||[points count]!=3)throw std::runtime_error("Physical water triangle points invalid");
      PhysicalWaterRegion::Triangle t;
      for(unsigned r=0;r<3;++r){id p=points[r];if(![p isKindOfClass:[NSArray class]]||[p count]!=3)throw std::runtime_error("Physical water point invalid");
        for(unsigned c=0;c<3;++c){if(![p[c] isKindOfClass:[NSNumber class]])throw std::runtime_error("Physical water scalar invalid");t[r][c]=[p[c] floatValue];}}
      triangles.push_back(t);
    }region.set(std::move(triangles));
    std::printf("[FO2 PHYSICAL WATER] collision=%u loaded %lu candidate triangles; static window/terrain geometry gates enabled\n",next-1,(unsigned long)[list count]);
  }
public:
  explicit PhysicalWaterBridge(bool enabled=false):on(enabled){}
  void disable(){on=false;region.set({});meshes.clear();results.clear();cachedBytes=0;}
  bool match(const Draw& d,std::array<float,3>& normal,std::shared_ptr<Buffer>& indices){
    if(!on||!d.captureMaterial||!d.vs||!d.ps||!d.vs->physicalWaterFamily||!d.ps->physicalWaterFamily||d.ps->physicalWaterFamily>2||
       d.vs->physicalWaterFamily!=d.ps->physicalWaterFamily||!d.debugProjection.valid||!d.depth||!d.raster.depthOn||d.raster.stencilOn)return false;
    ++attempts;++familyAttempts[d.ps->physicalWaterFamily];
    try{
      uint32_t current=d.physicalWaterAsset;if(asset!=current)load(current);if(!current||region.empty())return false;
      auto world=d.vs->physicalWaterFamily==1?waterPackedMatrix(d,4):physicalWaterBakedWorld(d);normal=physicalWaterNormal(d,world);
      auto raw=meshes.prepare(d,{100000,16384,33333});if(raw.status!=TraceMeshStatus::Ready)return false;
      auto pointer=raw.mesh.get();std::string key(reinterpret_cast<const char*>(&pointer),sizeof pointer);key.append(reinterpret_cast<const char*>(world.data()),sizeof world);
      auto found=results.find(key);
      if(found!=results.end()&&found->second.mesh.lock()==raw.mesh)indices=found->second.indices;
      else {
        auto filtered=region.filter(*raw.mesh,world);
        if(!filtered.empty()){
          auto sources=traceSourceIndices(d,raw.mesh->positions.size());indices=std::make_shared<Buffer>();indices->bytes.resize(filtered.size()*4);
          for(size_t i=0;i<filtered.size();++i){uint32_t value=sources.at(filtered[i]);std::memcpy(indices->bytes.data()+i*4,&value,4);}
        }
        size_t bytes=indices?indices->bytes.size():0;
        if(results.size()>=256||cachedBytes+bytes>16*1024*1024){results.clear();cachedBytes=0;}
        auto old=results.find(key);if(old!=results.end()&&old->second.indices)cachedBytes-=old->second.indices->bytes.size();
        results[key]={raw.mesh,indices};cachedBytes+=bytes;
      }
      if(indices){++accepted;++familyAccepted[d.ps->physicalWaterFamily];selectedTriangles+=indices->bytes.size()/12;}else ++rejected;return bool(indices);
    }catch(const std::exception& e){
      // Unsupported individual matrices/buffers are ordinary rejected draws.
      // A catalog failure is logged once for this identity and leaves it empty.
      if(errors++<8)std::fprintf(stderr,"[FO2 PHYSICAL WATER] rejected: %s\n",e.what());++rejected;return false;
    }
  }
  void report(uint64_t frame){if(on&&frame%300==0){std::printf("[FO2 PHYSICAL WATER] frame=%llu collision=%u attempts=%llu accepted=%llu rejected=%llu cached=%zu window=%llu/%llu terrain=%llu/%llu selected-triangles=%llu\n",
    (unsigned long long)frame,asset&&asset!=UINT32_MAX?asset-1:UINT32_MAX,(unsigned long long)attempts,(unsigned long long)accepted,(unsigned long long)rejected,results.size(),(unsigned long long)familyAccepted[1],(unsigned long long)familyAttempts[1],(unsigned long long)familyAccepted[2],(unsigned long long)familyAttempts[2],(unsigned long long)selectedTriangles);attempts=accepted=rejected=selectedTriangles=0;familyAttempts={};familyAccepted={};}}
};
}
