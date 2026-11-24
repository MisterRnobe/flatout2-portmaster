#pragma once
#include "../render/live_trace_scene.h"
#include "../render/trace_car_worklist.h"
#include <functional>
namespace fo2render {
using TraceTextureResolver=std::function<id<MTLTexture>(const std::shared_ptr<Image>&,bool)>;
using TraceSamplerResolver=std::function<id<MTLSamplerState>(const Sampler&)>;
struct TraceCarGPU {
  LiveTraceCarRange range;
  id<MTLBuffer> inputs,varyings;
  std::array<id<MTLTexture>,4> textures;
  std::array<id<MTLSamplerState>,4> samplers;
};
class TraceCarShading {
  id<MTLDevice> device;id<MTLComputePipelineState> vertex,shade;
  id<MTLComputePipelineState> countHits,prefixHits,scatterHits;
  bool compact=true;
  uint64_t lastCandidates=0,lastBaselineCandidates=0;
public:
  explicit TraceCarShading(id<MTLDevice> d):device(d){const char* flag=std::getenv("FO2_RT_CAR_WORKLIST");compact=!flag||std::strcmp(flag,"0");}
  void useWorklist(bool enabled){compact=enabled;}
  bool worklistEnabled()const{return compact;}
  uint64_t candidateThreads()const{return lastCandidates;}
  uint64_t baselineThreads()const{return lastBaselineCandidates;}
  void compile(const Shader& vs,const Shader& ps){
    if(vertex&&shade)return;
    auto source=traceCarMaterialSource(vs,ps);auto options=[MTLCompileOptions new];options.languageVersion=MTLLanguageVersion2_4;NSError* error=nil;
    auto library=[device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options error:&error];
    if(!library)throw std::runtime_error(std::string("RT car material MSL: ")+(error?error.localizedDescription.UTF8String:"unknown"));
    vertex=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"traceCarVertices"] error:&error];
    shade=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"traceCarShade"] error:&error];
    countHits=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"traceCarCount"] error:&error];
    prefixHits=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"traceCarPrefix"] error:&error];
    scatterHits=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"traceCarScatter"] error:&error];
    traceRequire(vertex&&shade&&countHits&&prefixHits&&scatterHits,"RT car material pipeline allocation failed");
  }
  std::vector<TraceCarGPU> prepare(const LiveTraceSnapshot& snapshot,const TraceTextureResolver& texture,const TraceSamplerResolver& sampler){
    std::vector<TraceCarGPU> result;if(!texture||!sampler)return result;
    size_t budget=0;
    for(const auto& range:snapshot.cars){auto& m=*range.material;
      if(!m.current()||range.vertices!=m.inputs.size()||!range.vertices||range.vertices>16384||!range.triangles||
         range.firstTriangle>snapshot.scene.indices.size()/3||range.triangles>snapshot.scene.indices.size()/3-range.firstTriangle||
         range.baseVertex>snapshot.scene.positions.size()||range.vertices>snapshot.scene.positions.size()-range.baseVertex)continue;
      size_t bytes=m.inputs.size()*(64+96);if(result.size()>=64||bytes>24*1024*1024-budget)continue;
      compile(*m.vs,*m.ps);TraceCarGPU item;item.range=range;
      item.inputs=[device newBufferWithBytes:m.inputs.data() length:m.inputs.size()*64 options:MTLResourceStorageModeShared];
      item.varyings=[device newBufferWithLength:m.inputs.size()*96 options:MTLResourceStorageModePrivate];
      traceRequire(item.inputs&&item.varyings,"RT car vertex allocation failed");
      for(unsigned i=0;i<4;++i){item.textures[i]=texture(m.textures[i],bool(m.srgbMask&(1<<i)));item.samplers[i]=sampler(m.samplers[i]);
        traceRequire(item.textures[i]&&item.samplers[i],"RT car texture/sampler missing");}
      result.push_back(std::move(item));budget+=bytes;
    }
    return result;
  }
  id<MTLBuffer> encode(id<MTLCommandBuffer> command,const std::vector<TraceCarGPU>& items,id<MTLBuffer> indices,id<MTLBuffer> hits,
      id<MTLBuffer> counters,id<MTLTexture> output){
    lastCandidates=lastBaselineCandidates=0;if(items.empty())return nil;
    const uint32_t pixels=uint32_t(output.width*output.height),triangles=uint32_t(indices.length/12),materials=uint32_t(items.size());
    traceRequire(pixels>0&&pixels<=2048*2048&&materials<=64,"RT car worklist dimensions");
    std::vector<std::array<uint32_t,2>> ranges;for(auto& item:items)ranges.push_back({item.range.firstTriangle,item.range.triangles});
    auto owners=traceCarPrimitiveOwners(triangles,ranges);
    uint32_t limits[]={pixels,triangles,materials,uint32_t(std::min(NSUInteger(64),shade.maxTotalThreadsPerThreadgroup))};
    lastBaselineCandidates=uint64_t(pixels)*materials;
    std::vector<uint32_t> zeros(materials*4);
    auto work=[device newBufferWithBytes:zeros.data() length:materials*16 options:MTLResourceStorageModeShared];
    auto active=[device newBufferWithLength:compact?size_t(pixels)*4:4 options:MTLResourceStorageModePrivate];
    auto ownerBuffer=compact?[device newBufferWithBytes:owners.data() length:owners.size()*4 options:MTLResourceStorageModeShared]:nil;
    auto histogram=compact?[device newBufferWithBytes:zeros.data() length:materials*8 options:MTLResourceStorageModeShared]:nil;
    auto indirect=compact?[device newBufferWithLength:materials*12 options:MTLResourceStorageModePrivate]:nil;
    traceRequire(work&&active&&(!compact||(ownerBuffer&&histogram&&indirect)),"RT car worklist allocation failed");
    if(compact){
      auto e=[command computeCommandEncoder];traceRequire(e!=nil,"RT car histogram encoder missing");[e setComputePipelineState:countHits];
      [e setBuffer:hits offset:0 atIndex:0];[e setBuffer:ownerBuffer offset:0 atIndex:1];[e setBuffer:histogram offset:0 atIndex:2];[e setBytes:limits length:sizeof limits atIndex:3];
      [e dispatchThreads:MTLSizeMake(pixels,1,1) threadsPerThreadgroup:MTLSizeMake(std::min(NSUInteger(64),countHits.maxTotalThreadsPerThreadgroup),1,1)];[e endEncoding];
      e=[command computeCommandEncoder];traceRequire(e!=nil,"RT car prefix encoder missing");[e setComputePipelineState:prefixHits];
      [e setBuffer:histogram offset:0 atIndex:0];[e setBuffer:work offset:0 atIndex:1];[e setBuffer:indirect offset:0 atIndex:2];[e setBytes:limits length:sizeof limits atIndex:3];
      [e dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];[e endEncoding];
      e=[command computeCommandEncoder];traceRequire(e!=nil,"RT car scatter encoder missing");[e setComputePipelineState:scatterHits];
      [e setBuffer:hits offset:0 atIndex:0];[e setBuffer:ownerBuffer offset:0 atIndex:1];[e setBuffer:histogram offset:materials*4 atIndex:2];
      [e setBuffer:work offset:0 atIndex:3];[e setBuffer:active offset:0 atIndex:4];[e setBytes:limits length:sizeof limits atIndex:5];
      [e dispatchThreads:MTLSizeMake(pixels,1,1) threadsPerThreadgroup:MTLSizeMake(std::min(NSUInteger(64),scatterHits.maxTotalThreadsPerThreadgroup),1,1)];[e endEncoding];
    }
    // Separate encoders establish the read/write dependency between trace,
    // vertex evaluation and material shading with ordinary tracked resources.
    auto e=[command computeCommandEncoder];traceRequire(e!=nil,"RT car vertex encoder missing");[e setComputePipelineState:vertex];
    for(auto& item:items){auto& m=*item.range.material;
      [e setBytes:m.vsConstants.data() length:m.vsConstants.size() atIndex:0];[e setBytes:m.effects.data() length:256 atIndex:2];
      [e setBuffer:item.inputs offset:0 atIndex:3];[e setBuffer:item.varyings offset:0 atIndex:4];
      [e dispatchThreads:MTLSizeMake(item.range.vertices,1,1) threadsPerThreadgroup:MTLSizeMake(std::min(NSUInteger(64),vertex.maxTotalThreadsPerThreadgroup),1,1)];
    }[e endEncoding];
    e=[command computeCommandEncoder];traceRequire(e!=nil,"RT car shade encoder missing");[e setComputePipelineState:shade];
    [e setBuffer:indices offset:0 atIndex:4];[e setBuffer:hits offset:0 atIndex:5];[e setBuffer:counters offset:0 atIndex:7];[e setTexture:output atIndex:4];
    [e setBuffer:active offset:0 atIndex:8];[e setBuffer:work offset:0 atIndex:9];uint32_t materialIndex=0;
    for(auto& item:items){auto& r=item.range;auto& m=*r.material;uint32_t range[]={r.firstTriangle,r.triangles,r.baseVertex,r.vertices};
      [e setBytes:m.psConstants.data() length:m.psConstants.size() atIndex:1];[e setBytes:m.effects.data() length:256 atIndex:2];
      [e setBuffer:item.varyings offset:0 atIndex:3];[e setBytes:range length:sizeof range atIndex:6];
      for(unsigned i=0;i<4;++i){[e setTexture:item.textures[i] atIndex:i];[e setSamplerState:item.samplers[i] atIndex:i];}
      [e setBytes:&materialIndex length:4 atIndex:10];
      if(compact)[e dispatchThreadgroupsWithIndirectBuffer:indirect indirectBufferOffset:materialIndex*12 threadsPerThreadgroup:MTLSizeMake(limits[3],1,1)];
      else [e dispatchThreads:MTLSizeMake(output.width,output.height,1) threadsPerThreadgroup:MTLSizeMake(limits[3],1,1)];
      ++materialIndex;
    }[e endEncoding];return work;
  }
  // Called after the owning RT command has completed; never read GPU counters
  // while their command is in flight. Count includes padded/empty dispatches.
  void completed(id<MTLBuffer> work,size_t materials){
    if(!compact){lastCandidates=lastBaselineCandidates;return;}
    if(!work)return;traceRequire(materials<=64&&work.length>=materials*16,"RT car worklist readback bounds");
    auto rows=static_cast<const uint32_t*>(work.contents);uint64_t used=0;
    uint32_t threads=uint32_t(std::min(NSUInteger(64),shade.maxTotalThreadsPerThreadgroup));
    for(size_t i=0;i<materials;++i){traceRequire(rows[i*4]==used&&rows[i*4+2]==1,"RT car worklist partition mismatch");
      used+=rows[i*4+1];traceRequire(used<=lastBaselineCandidates/materials,"RT car worklist pixel overflow");
      lastCandidates+=std::max(uint64_t(1),(uint64_t(rows[i*4+1])+threads-1)/threads)*threads;
    }
  }
};
}
