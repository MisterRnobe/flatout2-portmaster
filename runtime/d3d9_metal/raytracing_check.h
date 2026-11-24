#pragma once
// Diagnostic-only Metal RT path, compiled into renderer.mm. No game draw hooks.
#include "../render/raytracing_fixture.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <cstdio>
namespace fo2render {
inline const char* rayTracingCheckSource=R"MSL(
#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using namespace raytracing;
struct TraceRay {float4 originMin;float4 directionMax;uint4 mask;};
struct TraceHit {float distance;uint primitive;uint instance;uint valid;float4 barycentric;};
kernel void traceFixture(instance_acceleration_structure scene [[buffer(0)]],
                         device const TraceRay* rays [[buffer(1)]],
                         device TraceHit* hits [[buffer(2)]],uint i [[thread_position_in_grid]]){
    TraceRay input=rays[i];ray r;
    r.origin=input.originMin.xyz;r.min_distance=input.originMin.w;
    r.direction=input.directionMax.xyz;r.max_distance=input.directionMax.w;
    intersector<triangle_data,instancing> query;
    query.assume_geometry_type(geometry_type::triangle);
    query.accept_any_intersection(false);
    auto result=query.intersect(r,scene,input.mask.x);
    TraceHit hit;hit.distance=0;hit.primitive=0xffffffff;hit.instance=0xffffffff;
    hit.valid=0;hit.barycentric=float4(0);
    if(result.type==intersection_type::triangle){
        hit.distance=result.distance;hit.primitive=result.primitive_id;hit.instance=result.instance_id;
        hit.valid=1;hit.barycentric.xy=result.triangle_barycentric_coord;
    }
    hits[i]=hit;
}
)MSL";
inline void traceRequire(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
inline void traceWait(id<MTLCommandBuffer> command){
  traceRequire(command!=nil,"RT command buffer allocation failed");
  [command commit];[command waitUntilCompleted];
  if(command.status!=MTLCommandBufferStatusCompleted)
    throw std::runtime_error(std::string("RT GPU command failed: ")+
      (command.error?command.error.localizedDescription.UTF8String:"unknown error"));
}
inline id<MTLAccelerationStructure> buildTraceStructure(id<MTLDevice> device,id<MTLCommandQueue> queue,
                                                       MTLAccelerationStructureDescriptor* descriptor){
  auto sizes=[device accelerationStructureSizesWithDescriptor:descriptor];
  traceRequire(sizes.accelerationStructureSize>0,"RT acceleration structure has zero size");
  auto structure=[device newAccelerationStructureWithSize:sizes.accelerationStructureSize];
  auto scratch=[device newBufferWithLength:std::max(NSUInteger(1),sizes.buildScratchBufferSize) options:MTLResourceStorageModePrivate];
  traceRequire(structure!=nil&&scratch!=nil,"RT acceleration structure/scratch allocation failed");
  auto command=[queue commandBuffer];auto encoder=[command accelerationStructureCommandEncoder];
  traceRequire(encoder!=nil,"RT acceleration structure encoder allocation failed");
  [encoder buildAccelerationStructure:structure descriptor:descriptor scratchBuffer:scratch scratchBufferOffset:0];
  [encoder endEncoding];traceWait(command);
  return structure;
}
int checkMetalRayTracing(bool gpu){
  validateTraceFixture();
  std::puts("[FO2 RT] CPU fixture passed: nearest/transformed hits, masks, ray ranges and barycentrics");
  if(!gpu)return 0;
  @autoreleasepool {
    NSMutableDictionary* report=[@{@"mode":@"diagnostic-only",@"status":@"pending",@"gameplay_rt_enabled":@NO} mutableCopy];
    auto save=[&]{
      const char* session=std::getenv("FO2_METAL_SESSION_DIR");
      auto path=(session&&*session?std::filesystem::path(session):std::filesystem::path("work/metal"))/"raytracing-check.json";
      std::filesystem::create_directories(path.parent_path());
      NSError* error=nil;NSData* json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:&error];
      if(!json||![json writeToFile:[NSString stringWithUTF8String:path.c_str()] options:NSDataWritingAtomic error:&error])
        throw std::runtime_error(std::string("RT report write failed: ")+(error?error.localizedDescription.UTF8String:"unknown error"));
      std::printf("[FO2 RT] Report: %s\n",path.c_str());
    };
    try {
      id<MTLDevice> device=MTLCreateSystemDefaultDevice();
      if(!device){report[@"status"]=@"no-device";save();std::puts("[FO2 RT] No Metal device in this session; GPU check skipped");return 77;}
      report[@"device"]=device.name;report[@"api_supported"]=@(device.supportsRaytracing);
      const bool apple9=[device supportsFamily:MTLGPUFamilyApple9];
      report[@"apple_family_9_or_later"]=@(apple9);
      std::printf("[FO2 RT] Device: %s; RT API=%d; Apple family 9+=%d\n",device.name.UTF8String,int(device.supportsRaytracing),int(apple9));
      if(!device.supportsRaytracing){report[@"status"]=@"unsupported";save();std::puts("[FO2 RT] RT API unsupported; gameplay continues using SSR");return 77;}
      auto queue=[device newCommandQueue];traceRequire(queue!=nil,"RT command queue allocation failed");
      auto options=[MTLCompileOptions new];options.languageVersion=MTLLanguageVersion2_4;
      NSError* error=nil;
      auto library=[device newLibraryWithSource:[NSString stringWithUTF8String:rayTracingCheckSource] options:options error:&error];
      if(!library)throw std::runtime_error(std::string("RT MSL compilation failed: ")+(error?error.localizedDescription.UTF8String:"unknown error"));
      auto function=[library newFunctionWithName:@"traceFixture"];
      traceRequire(function!=nil,"RT kernel entry point missing");
      auto pipeline=[device newComputePipelineStateWithFunction:function error:&error];
      if(!pipeline)throw std::runtime_error(std::string("RT compute pipeline failed: ")+(error?error.localizedDescription.UTF8String:"unknown error"));
      auto rays=traceRays();
      auto vertices=[device newBufferWithBytes:traceVertices length:sizeof(traceVertices) options:MTLResourceStorageModeShared];
      auto inputs=[device newBufferWithBytes:rays.data() length:sizeof(rays) options:MTLResourceStorageModeShared];
      auto outputs=[device newBufferWithLength:sizeof(TraceHit)*rays.size() options:MTLResourceStorageModeShared];
      traceRequire(vertices!=nil&&inputs!=nil&&outputs!=nil,"RT fixture buffer allocation failed");
      NSMutableArray* formats=[NSMutableArray array];
      auto start=std::chrono::steady_clock::now();
      for(bool wide:{false,true}){
        auto indices=[device newBufferWithBytes:wide?static_cast<const void*>(traceIndices32):static_cast<const void*>(traceIndices16)
                                       length:wide?sizeof(traceIndices32):sizeof(traceIndices16) options:MTLResourceStorageModeShared];
        traceRequire(indices!=nil,"RT index buffer allocation failed");
        auto geometry=[MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
        geometry.vertexBuffer=vertices;geometry.vertexBufferOffset=16;geometry.vertexStride=16;geometry.vertexFormat=MTLAttributeFormatFloat3;
        geometry.indexBuffer=indices;geometry.indexBufferOffset=4;geometry.indexType=wide?MTLIndexTypeUInt32:MTLIndexTypeUInt16;
        geometry.triangleCount=1;geometry.opaque=YES;
        auto primitiveDescriptor=[MTLPrimitiveAccelerationStructureDescriptor descriptor];primitiveDescriptor.geometryDescriptors=@[geometry];
        auto primitive=buildTraceStructure(device,queue,primitiveDescriptor);
        MTLAccelerationStructureInstanceDescriptor instances[2]{};
        for(unsigned i=0;i<2;++i){
          instances[i].transformationMatrix.columns[0]=MTLPackedFloat3Make(1,0,0);
          instances[i].transformationMatrix.columns[1]=MTLPackedFloat3Make(0,1,0);
          instances[i].transformationMatrix.columns[2]=MTLPackedFloat3Make(0,0,1);
          instances[i].transformationMatrix.columns[3]=MTLPackedFloat3Make(0,0,float(i*2));
          instances[i].mask=1u<<i;instances[i].accelerationStructureIndex=0;
        }
        auto instanceBuffer=[device newBufferWithBytes:instances length:sizeof(instances) options:MTLResourceStorageModeShared];
        traceRequire(instanceBuffer!=nil,"RT instance buffer allocation failed");
        auto instanceDescriptor=[MTLInstanceAccelerationStructureDescriptor descriptor];
        instanceDescriptor.instanceCount=2;instanceDescriptor.instanceDescriptorBuffer=instanceBuffer;
        instanceDescriptor.instancedAccelerationStructures=@[primitive];
        auto scene=buildTraceStructure(device,queue,instanceDescriptor);
        std::memset(outputs.contents,0xcd,outputs.length);
        auto command=[queue commandBuffer];auto encoder=[command computeCommandEncoder];
        traceRequire(encoder!=nil,"RT compute encoder allocation failed");
        [encoder setComputePipelineState:pipeline];[encoder setAccelerationStructure:scene atBufferIndex:0];
        [encoder useResource:primitive usage:MTLResourceUsageRead];
        [encoder setBuffer:inputs offset:0 atIndex:1];[encoder setBuffer:outputs offset:0 atIndex:2];
        [encoder dispatchThreads:MTLSizeMake(rays.size(),1,1) threadsPerThreadgroup:MTLSizeMake(std::min(NSUInteger(rays.size()),pipeline.maxTotalThreadsPerThreadgroup),1,1)];
        [encoder endEncoding];traceWait(command);
        auto actual=static_cast<const TraceHit*>(outputs.contents);unsigned hits=0;
        NSMutableArray* results=[NSMutableArray array];
        for(unsigned i=0;i<rays.size();++i){
          auto expected=traceReference(rays[i]);
          if(!traceHitMatches(actual[i],expected))throw std::runtime_error("RT GPU/reference mismatch for "+std::to_string(wide?32:16)+"-bit indices, ray "+std::to_string(i));
          hits+=actual[i].valid;
          [results addObject:@{@"ray":@(i),@"hit":@(actual[i].valid),@"distance":@(actual[i].distance),@"instance":@(actual[i].instance),@"primitive":@(actual[i].primitive),@"u":@(actual[i].barycentric[0]),@"v":@(actual[i].barycentric[1])}];
        }
        [formats addObject:@{@"index_bits":@(wide?32:16),@"hits":@(hits),@"rays":results}];
        std::printf("[FO2 RT] %u-bit indices: %zu rays, %u hits; transforms, masks, nearest distances and barycentrics passed\n",wide?32:16,rays.size(),hits);
      }
      report[@"formats"]=formats;
      report[@"fixture_wall_ms"]=@(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
      report[@"status"]=@"passed";save();return 0;
    }catch(const std::exception& error){
      report[@"status"]=@"failed";report[@"error"]=[NSString stringWithUTF8String:error.what()];save();throw;
    }
  }
}
}
