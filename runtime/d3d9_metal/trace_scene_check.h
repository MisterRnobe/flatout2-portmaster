#pragma once
// Included after raytracing_check.h in renderer.mm. Snapshot diagnostic only.
#include "../render/trace_scene.h"
#include <fstream>
namespace fo2render {
inline std::vector<uint8_t> traceSceneFile(const std::filesystem::path& path,size_t limit){
  auto size=std::filesystem::file_size(path);
  traceRequire(size>0&&size<=limit,"RT scene file empty or exceeds limit");
  std::vector<uint8_t> bytes(size);std::ifstream stream(path,std::ios::binary);
  traceRequire(bool(stream.read(reinterpret_cast<char*>(bytes.data()),bytes.size())),"RT scene file read failed");return bytes;
}
inline TraceScene loadTraceScene(const std::filesystem::path& root,NSDictionary** metadata){
  auto bytes=traceSceneFile(root/"scene.json",1024*1024);
  NSError* error=nil;auto data=[NSData dataWithBytes:bytes.data() length:bytes.size()];
  id document=[NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
  traceRequire([document isKindOfClass:[NSDictionary class]],"Invalid RT scene manifest JSON");
  NSDictionary* doc=document;
  auto number=[&](NSString* name,uint64_t limit){
    id v=doc[name];traceRequire([v isKindOfClass:[NSNumber class]],"RT scene manifest number missing");
    double n=[v doubleValue];traceRequire(std::isfinite(n)&&n>=0&&n<=limit&&std::floor(n)==n,"RT scene manifest number out of range");return size_t(n);
  };
  traceRequire(number(@"schema",1)==1&&[doc[@"status"] isEqual:@"cpu-prepared"]&&
    [doc[@"coordinate_space"] isEqual:@"right-handed-view"]&&[doc[@"coverage"] isEqual:@"submitted-captured-draws-only"],"Unsupported RT snapshot schema/coordinate space");
  auto vertexCount=number(@"vertices",traceSceneMaxVertices),triangleCount=number(@"triangles",traceSceneMaxTriangles);number(@"source_frame",UINT32_MAX);
  traceRequire(vertexCount&&triangleCount,"Empty RT scene");
  id projection=doc[@"projection"];traceRequire([projection isKindOfClass:[NSArray class]]&&[projection count]==4,"RT scene projection missing");
  TraceScene scene;
  for(unsigned k=0;k<4;++k){id v=projection[k];traceRequire([v isKindOfClass:[NSNumber class]],"RT scene projection component invalid");scene.projection[k]=[v floatValue];}
  auto positions=traceSceneFile(root/"positions.f32",traceSceneMaxVertices*12),indices=traceSceneFile(root/"indices.u32",traceSceneMaxTriangles*12),ids=traceSceneFile(root/"triangle-mesh.u32",traceSceneMaxTriangles*4);
  traceRequire(positions.size()==vertexCount*12&&indices.size()==triangleCount*12&&ids.size()==triangleCount*4,"RT scene file counts disagree with manifest");
  static_assert(sizeof(std::array<float,3>)==12);
  scene.positions.resize(vertexCount);scene.indices.resize(triangleCount*3);scene.meshIds.resize(triangleCount);
  std::memcpy(scene.positions.data(),positions.data(),positions.size());std::memcpy(scene.indices.data(),indices.data(),indices.size());std::memcpy(scene.meshIds.data(),ids.data(),ids.size());
  validateTraceScene(scene);*metadata=doc;return scene;
}
inline int checkPreparedTraceScene(const char* directory,bool gpu,const TraceScene* prepared=nullptr,
                                  NSDictionary* preparedMetadata=nil,const std::filesystem::path* outputOverride=nullptr){
  @autoreleasepool {
    const char* session=std::getenv("FO2_METAL_SESSION_DIR");
    auto output=outputOverride?*outputOverride:(session&&*session?std::filesystem::path(session):std::filesystem::path("work/metal/rt-scene-check"));
    std::filesystem::create_directories(output);
    NSMutableDictionary* report=[@{@"mode":@"captured-view-scene-diagnostic",@"status":@"pending",@"gameplay_rt_enabled":@NO,@"world_space_validated":@NO,@"native_rt_gpu_validated":@NO} mutableCopy];
    auto save=[&]{NSError* error=nil;auto bytes=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:&error];
      traceRequire(bytes&&[bytes writeToFile:[NSString stringWithUTF8String:(output/"raytracing-scene-check.json").c_str()] options:NSDataWritingAtomic error:&error],"RT scene report write failed");};
    try{
      traceRequire(directory&&*directory,"RT scene directory argument required");
      NSDictionary* metadata=preparedMetadata;TraceScene loaded;
      if(!prepared){loaded=loadTraceScene(directory,&metadata);prepared=&loaded;}
      const auto& scene=*prepared;validateTraceScene(scene);
      report[@"source"]=[NSString stringWithUTF8String:directory];report[@"source_frame"]=metadata[@"source_frame"];
      report[@"source_kind"]=preparedMetadata?@"current-frame-owned-memory":@"prepared-file-snapshot";
      report[@"coverage"]=@"submitted-captured-draws-only";report[@"vertices"]=@(scene.positions.size());report[@"triangles"]=@(scene.indices.size()/3);
      auto probes=traceSceneProbes(scene);std::vector<TraceHit> expected;unsigned expectedHits=0;
      for(const auto& r:probes){auto hit=traceSceneReference(scene,r);expectedHits+=hit.valid;expected.push_back(hit);}
      traceRequire(expectedHits>0,"RT scene probes did not hit any captured geometry");
      report[@"reference_rays"]=@(probes.size());report[@"reference_hits"]=@(expectedHits);
      std::printf("[FO2 RT SCENE] CPU: %zu vertices, %zu triangles, %zu probes, %u hits\n",scene.positions.size(),scene.indices.size()/3,probes.size(),expectedHits);
      if(!gpu){report[@"status"]=@"cpu-validated";save();return 0;}
      id<MTLDevice> device=MTLCreateSystemDefaultDevice();
      if(!device||!device.supportsRaytracing){report[@"status"]=device?@"unsupported":@"no-device";save();std::puts("[FO2 RT SCENE] RT device unavailable; gameplay remains SSR");return 77;}
      report[@"device"]=device.name;
      auto start=std::chrono::steady_clock::now();auto queue=[device newCommandQueue];traceRequire(queue!=nil,"RT scene queue allocation failed");
      auto vertices=[device newBufferWithBytes:scene.positions.data() length:scene.positions.size()*12 options:MTLResourceStorageModeShared];
      auto indices=[device newBufferWithBytes:scene.indices.data() length:scene.indices.size()*4 options:MTLResourceStorageModeShared];
      traceRequire(vertices&&indices,"RT scene geometry buffer allocation failed");
      auto geometry=[MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
      geometry.vertexBuffer=vertices;geometry.vertexStride=12;geometry.vertexFormat=MTLAttributeFormatFloat3;
      geometry.indexBuffer=indices;geometry.indexType=MTLIndexTypeUInt32;geometry.triangleCount=scene.indices.size()/3;geometry.opaque=YES;
      auto primitiveDescriptor=[MTLPrimitiveAccelerationStructureDescriptor descriptor];primitiveDescriptor.geometryDescriptors=@[geometry];
      auto primitive=buildTraceStructure(device,queue,primitiveDescriptor);
      MTLAccelerationStructureInstanceDescriptor instance{};
      instance.transformationMatrix.columns[0]=MTLPackedFloat3Make(1,0,0);
      instance.transformationMatrix.columns[1]=MTLPackedFloat3Make(0,1,0);
      instance.transformationMatrix.columns[2]=MTLPackedFloat3Make(0,0,1);instance.mask=255;
      auto instanceBuffer=[device newBufferWithBytes:&instance length:sizeof(instance) options:MTLResourceStorageModeShared];
      traceRequire(instanceBuffer!=nil,"RT scene instance buffer allocation failed");
      auto descriptor=[MTLInstanceAccelerationStructureDescriptor descriptor];descriptor.instanceCount=1;
      descriptor.instanceDescriptorBuffer=instanceBuffer;descriptor.instancedAccelerationStructures=@[primitive];
      auto acceleration=buildTraceStructure(device,queue,descriptor);
      report[@"build_wall_ms"]=@(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
      constexpr unsigned width=160,height=100;
      std::vector<TraceRay> rays;for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)rays.push_back(traceCameraRay(scene,x,y,width,height));
      rays.insert(rays.end(),probes.begin(),probes.end());
      auto inputs=[device newBufferWithBytes:rays.data() length:rays.size()*sizeof(TraceRay) options:MTLResourceStorageModeShared];
      auto outputs=[device newBufferWithLength:rays.size()*sizeof(TraceHit) options:MTLResourceStorageModeShared];
      traceRequire(inputs&&outputs,"RT scene ray buffer allocation failed");std::memset(outputs.contents,0xcd,outputs.length);
      NSError* error=nil;auto options=[MTLCompileOptions new];options.languageVersion=MTLLanguageVersion2_4;
      auto library=[device newLibraryWithSource:[NSString stringWithUTF8String:rayTracingCheckSource] options:options error:&error];
      if(!library)throw std::runtime_error(std::string("RT scene MSL failed: ")+(error?error.localizedDescription.UTF8String:"unknown"));
      auto function=[library newFunctionWithName:@"traceFixture"];traceRequire(function!=nil,"RT scene kernel missing");
      auto pipeline=[device newComputePipelineStateWithFunction:function error:&error];traceRequire(pipeline!=nil,"RT scene pipeline creation failed");
      auto command=[queue commandBuffer];auto encoder=[command computeCommandEncoder];traceRequire(encoder!=nil,"RT scene compute encoder missing");
      [encoder setComputePipelineState:pipeline];[encoder setAccelerationStructure:acceleration atBufferIndex:0];[encoder useResource:primitive usage:MTLResourceUsageRead];
      [encoder setBuffer:inputs offset:0 atIndex:1];[encoder setBuffer:outputs offset:0 atIndex:2];
      [encoder dispatchThreads:MTLSizeMake(rays.size(),1,1) threadsPerThreadgroup:MTLSizeMake(std::min(NSUInteger(64),pipeline.maxTotalThreadsPerThreadgroup),1,1)];
      [encoder endEncoding];traceWait(command);
      auto actual=static_cast<const TraceHit*>(outputs.contents);unsigned imageHits=0;
      std::vector<uint8_t> colors(width*height*3),depthImage(colors.size());
      for(unsigned i=0;i<width*height;++i){const auto& h=actual[i];
        traceRequire(h.valid<=1,"RT scene invalid hit flag");if(!h.valid){traceRequire(traceHitMatches(h,TraceHit{}),"RT scene corrupt miss");continue;}
        traceRequire(h.instance==0&&h.primitive<scene.meshIds.size()&&std::isfinite(h.distance)&&h.distance>=rays[i].originMin[3]&&h.distance<=rays[i].directionMax[3]&&
          std::isfinite(h.barycentric[0])&&std::isfinite(h.barycentric[1])&&h.barycentric[0]>=-.001f&&h.barycentric[1]>=-.001f&&h.barycentric[0]+h.barycentric[1]<=1.001f,"RT scene invalid image hit");
        ++imageHits;auto id=scene.meshIds[h.primitive]+1;
        for(unsigned k=0;k<3;++k){colors[i*3+k]=uint8_t(48+(id*(37+k*42))%208);depthImage[i*3+k]=uint8_t(255/(1+.03f*h.distance));}
      }
      unsigned mismatch=0;NSMutableArray* probeResults=[NSMutableArray array];
      for(unsigned i=0;i<probes.size();++i)if(!traceSceneHitMatches(scene,probes[i],actual[width*height+i],expected[i])){
        ++mismatch;std::fprintf(stderr,"[FO2 RT SCENE] Probe %u mismatch: GPU hit=%u tri=%u distance=%g; CPU hit=%u tri=%u distance=%g\n",i,actual[width*height+i].valid,actual[width*height+i].primitive,actual[width*height+i].distance,expected[i].valid,expected[i].primitive,expected[i].distance);}
      for(unsigned i=0;i<probes.size();++i){const auto& h=actual[width*height+i];
        [probeResults addObject:@{@"ray":@(i),@"mask":@(probes[i].mask[0]),@"gpu_hit":@(h.valid),@"cpu_hit":@(expected[i].valid),
          @"gpu_primitive":@(h.primitive),@"cpu_primitive":@(expected[i].primitive),
          @"gpu_distance":std::isfinite(h.distance)?id(@(h.distance)):id([NSNull null]),@"cpu_distance":@(expected[i].distance)}];}
      report[@"probes"]=probeResults;
      report[@"reference_mismatches"]=@(mismatch);report[@"image_hits"]=@(imageHits);
      auto ppm=[&](const char* name,const auto& pixels){std::ofstream f(output/name,std::ios::binary);f<<"P6\n"<<width<<" "<<height<<"\n255\n";f.write(reinterpret_cast<const char*>(pixels.data()),pixels.size());traceRequire(bool(f),"RT scene image write failed");};
      ppm("rt-scene-meshes.ppm",colors);ppm("rt-scene-distance.ppm",depthImage);
      traceRequire(imageHits>0,"RT scene camera image contains no geometry");
      traceRequire(mismatch==0,"RT scene GPU/reference mismatch");
      report[@"status"]=@"passed";report[@"native_rt_gpu_validated"]=@YES;save();
      std::printf("[FO2 RT SCENE] GPU: %zu triangles, %u image hits; %zu nearest-hit/barycentric probes passed. Gameplay remains SSR.\n",scene.indices.size()/3,imageHits,probes.size());return 0;
    }catch(const std::exception& error){report[@"status"]=@"failed";report[@"error"]=[NSString stringWithUTF8String:error.what()];save();throw;}
  }
}
int checkMetalRayTracingScene(const char* directory,bool gpu){return checkPreparedTraceScene(directory,gpu);}
}
