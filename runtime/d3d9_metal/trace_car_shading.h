#pragma once
#include "../render/live_trace_scene.h"
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
public:
  explicit TraceCarShading(id<MTLDevice> d):device(d){}
  void compile(const Shader& vs,const Shader& ps){
    if(vertex&&shade)return;
    auto source=traceCarMaterialSource(vs,ps);auto options=[MTLCompileOptions new];options.languageVersion=MTLLanguageVersion2_4;NSError* error=nil;
    auto library=[device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options error:&error];
    if(!library)throw std::runtime_error(std::string("RT car material MSL: ")+(error?error.localizedDescription.UTF8String:"unknown"));
    vertex=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"traceCarVertices"] error:&error];
    shade=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"traceCarShade"] error:&error];
    traceRequire(vertex&&shade,"RT car material pipeline allocation failed");
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
  void encode(id<MTLCommandBuffer> command,const std::vector<TraceCarGPU>& items,id<MTLBuffer> indices,id<MTLBuffer> hits,
      id<MTLBuffer> counters,id<MTLTexture> output){
    if(items.empty())return;
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
    for(auto& item:items){auto& r=item.range;auto& m=*r.material;uint32_t range[]={r.firstTriangle,r.triangles,r.baseVertex,r.vertices};
      [e setBytes:m.psConstants.data() length:m.psConstants.size() atIndex:1];[e setBytes:m.effects.data() length:256 atIndex:2];
      [e setBuffer:item.varyings offset:0 atIndex:3];[e setBytes:range length:sizeof range atIndex:6];
      for(unsigned i=0;i<4;++i){[e setTexture:item.textures[i] atIndex:i];[e setSamplerState:item.samplers[i] atIndex:i];}
      [e dispatchThreads:MTLSizeMake(output.width,output.height,1) threadsPerThreadgroup:MTLSizeMake(std::min(NSUInteger(64),shade.maxTotalThreadsPerThreadgroup),1,1)];
    }[e endEncoding];
  }
};
}
