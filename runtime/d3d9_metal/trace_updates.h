#pragma once
#include "../render/trace_update_worker.h"
#include "trace_mismatch_dump.h"
namespace fo2render {
// Owned and accessed only by the worker. No structures are exposed to shading
// yet; all previous commands have completed before shared buffers are reused.
class TraceUpdateGPU {
  id<MTLDevice> device;id<MTLCommandQueue> queue;id<MTLComputePipelineState> pipeline;
  id<MTLBuffer> vertices,indices,instances,scratch,inputs,outputs;
  id<MTLAccelerationStructure> latestPrimitive,latestScene;
  size_t retainedAS=0,buildingAS=0,peakBytes=0;
  uint64_t updates=0,lastFingerprint=0;
  uint64_t lastFrame=0,comparisons=0,changedUpdates=0;
  double maxCaptureMs=0,maxWorkerMs=0,totalCaptureMs=0,totalBuildMs=0,lastCheckpointMs=0,maxPrepareMs=0,totalPrepareMs=0;
  uint64_t cacheHits=0,cacheMisses=0,precisionComparisons=0;
  NSDictionary* lastReport;
  std::filesystem::path directory;
  static constexpr size_t memoryLimit=128*1024*1024;
  size_t buffers()const{return vertices.length+indices.length+instances.length+scratch.length+inputs.length+outputs.length;}
  void budget(size_t extra){auto used=buffers()+retainedAS+buildingAS;traceRequire(used<=memoryLimit&&extra<=memoryLimit-used,"RT update GPU memory budget exceeded");peakBytes=std::max(peakBytes,used+extra);}
  void ensure(id<MTLBuffer> __strong& buffer,size_t bytes,MTLResourceOptions options){
    if(buffer&&buffer.length>=bytes)return;size_t capacity=256;while(capacity<bytes&&capacity<memoryLimit)capacity*=2;
    traceRequire(capacity>=bytes&&capacity<=64*1024*1024,"RT update buffer size exceeds limit");
    // During replacement both old and new allocations can be alive. Include
    // that transient peak before assigning/releasing the previous buffer.
    budget(capacity);
    buffer=[device newBufferWithLength:capacity options:options];traceRequire(buffer!=nil,"RT update buffer allocation failed");budget(0);
  }
  id<MTLAccelerationStructure> build(MTLAccelerationStructureDescriptor* descriptor){
    auto sizes=[device accelerationStructureSizesWithDescriptor:descriptor];traceRequire(sizes.accelerationStructureSize>0,"RT update empty acceleration structure");
    ensure(scratch,std::max(NSUInteger(1),sizes.buildScratchBufferSize),MTLResourceStorageModePrivate);budget(sizes.accelerationStructureSize);
    auto structure=[device newAccelerationStructureWithSize:sizes.accelerationStructureSize];traceRequire(structure!=nil,"RT update structure allocation failed");
    buildingAS+=sizes.accelerationStructureSize;auto command=[queue commandBuffer];auto encoder=[command accelerationStructureCommandEncoder];
    traceRequire(encoder!=nil,"RT update build encoder allocation failed");[encoder buildAccelerationStructure:structure descriptor:descriptor scratchBuffer:scratch scratchBufferOffset:0];
    [encoder endEncoding];traceWait(command);return structure;
  }
  void initialize(){
    if(device)return;device=MTLCreateSystemDefaultDevice();traceRequire(device&&device.supportsRaytracing,"RT updates unavailable on this device/session");
    queue=[device newCommandQueue];traceRequire(queue!=nil,"RT update queue allocation failed");
    auto options=[MTLCompileOptions new];options.languageVersion=MTLLanguageVersion2_4;NSError* error=nil;
    auto library=[device newLibraryWithSource:[NSString stringWithUTF8String:rayTracingCheckSource] options:options error:&error];
    if(!library)throw std::runtime_error(std::string("RT update MSL compilation failed: ")+(error?error.localizedDescription.UTF8String:"unknown"));
    auto function=[library newFunctionWithName:@"traceFixture"];traceRequire(function!=nil,"RT update kernel missing");
    pipeline=[device newComputePipelineStateWithFunction:function error:&error];traceRequire(pipeline!=nil,"RT update pipeline allocation failed");
  }
  void report(NSDictionary* doc){lastReport=doc;std::filesystem::create_directories(directory);NSError* error=nil;
    auto data=[NSJSONSerialization dataWithJSONObject:doc options:NSJSONWritingPrettyPrinted error:&error];
    traceRequire(data&&[data writeToFile:[NSString stringWithUTF8String:(directory/"progress.json").c_str()] options:NSDataWritingAtomic error:&error],"RT update progress report failed");}
public:
  TraceUpdateGPU(){const char* session=std::getenv("FO2_METAL_SESSION_DIR");directory=(session&&*session?std::filesystem::path(session):std::filesystem::path("work/metal"))/"rt-updates";}
  void invalidate(){@autoreleasepool{latestScene=nil;latestPrimitive=nil;retainedAS=buildingAS=0;lastFingerprint=0;
    if(lastReport&&![lastReport[@"status"] isEqual:@"failed"]){auto doc=[lastReport mutableCopy];doc[@"status"]=@"invalidated";doc[@"gpu_retained_as_bytes"]=@0;report(doc);}}}
  void finish(){@autoreleasepool{if(!lastReport)return;auto doc=[lastReport mutableCopy];
    doc[@"status"]=@"stopped";doc[@"precision_equivalent_comparisons"]=@(precisionComparisons);doc[@"mean_worker_prepare_ms"]=@(updates?totalPrepareMs/updates:0);doc[@"max_worker_prepare_ms"]=@(maxPrepareMs);doc[@"mesh_cache_total_hits"]=@(cacheHits);doc[@"mesh_cache_total_misses"]=@(cacheMisses);doc[@"updates"]=@(updates);doc[@"source_frame"]=@(lastFrame);doc[@"cpu_gpu_comparisons"]=@(comparisons);doc[@"changed_updates"]=@(changedUpdates);
    doc[@"max_capture_cpu_ms"]=@(maxCaptureMs);doc[@"max_worker_core_ms"]=@(maxWorkerMs);doc[@"mean_capture_cpu_ms"]=@(updates?totalCaptureMs/updates:0);doc[@"mean_build_wall_ms"]=@(updates?totalBuildMs/updates:0);doc[@"last_checkpoint_wall_ms"]=@(lastCheckpointMs);report(doc);}}
  void prepareAndProcess(const LiveTraceSnapshot& snapshot,const TraceUpdateWorker::Current& current){@autoreleasepool{
    if(!current())return;auto prepared=snapshot;
    try{finalizeLiveTraceSnapshot(prepared);}catch(const std::exception& e){invalidate();report(@{@"status":@"failed",@"error":[NSString stringWithUTF8String:e.what()],@"source_frame":@(snapshot.frame),@"gameplay_rt_enabled":@NO});throw;}
    if(current())process(prepared,current);
  }}
  void process(const LiveTraceSnapshot& snapshot,const TraceUpdateWorker::Current& current){@autoreleasepool{
    try{
      if(!current())return;
      if(snapshot.scene.indices.empty()){if(snapshot.sample==1||snapshot.sample%30==0)report(@{@"status":@"no-geometry",@"source_frame":@(snapshot.frame),@"unknown_programs":@(snapshot.unknown),@"invalid_draws":@(snapshot.invalid),@"gameplay_rt_enabled":@NO});return;}
      validateTraceScene(snapshot.scene);initialize();const auto& scene=snapshot.scene;
      auto start=std::chrono::steady_clock::now();buildingAS=0;
      ensure(vertices,scene.positions.size()*12,MTLResourceStorageModeShared);ensure(indices,scene.indices.size()*4,MTLResourceStorageModeShared);
      std::memcpy(vertices.contents,scene.positions.data(),scene.positions.size()*12);std::memcpy(indices.contents,scene.indices.data(),scene.indices.size()*4);
      auto geometry=[MTLAccelerationStructureTriangleGeometryDescriptor descriptor];geometry.vertexBuffer=vertices;geometry.vertexStride=12;geometry.vertexFormat=MTLAttributeFormatFloat3;
      geometry.indexBuffer=indices;geometry.indexType=MTLIndexTypeUInt32;geometry.triangleCount=scene.indices.size()/3;geometry.opaque=YES;
      auto primitiveDescriptor=[MTLPrimitiveAccelerationStructureDescriptor descriptor];primitiveDescriptor.geometryDescriptors=@[geometry];auto primitive=build(primitiveDescriptor);
      MTLAccelerationStructureInstanceDescriptor instance{};instance.transformationMatrix.columns[0]=MTLPackedFloat3Make(1,0,0);instance.transformationMatrix.columns[1]=MTLPackedFloat3Make(0,1,0);instance.transformationMatrix.columns[2]=MTLPackedFloat3Make(0,0,1);instance.mask=255;
      ensure(instances,sizeof(instance),MTLResourceStorageModeShared);std::memcpy(instances.contents,&instance,sizeof(instance));
      auto descriptor=[MTLInstanceAccelerationStructureDescriptor descriptor];descriptor.instanceCount=1;descriptor.instanceDescriptorBuffer=instances;descriptor.instancedAccelerationStructures=@[primitive];auto acceleration=build(descriptor);
      auto buildMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
      auto probes=traceSceneProbes(scene);ensure(inputs,probes.size()*sizeof(TraceRay),MTLResourceStorageModeShared);ensure(outputs,probes.size()*sizeof(TraceHit),MTLResourceStorageModeShared);
      std::memcpy(inputs.contents,probes.data(),probes.size()*sizeof(TraceRay));std::memset(outputs.contents,0xcd,probes.size()*sizeof(TraceHit));
      auto command=[queue commandBuffer];auto encoder=[command computeCommandEncoder];traceRequire(encoder!=nil,"RT update trace encoder missing");
      [encoder setComputePipelineState:pipeline];[encoder setAccelerationStructure:acceleration atBufferIndex:0];[encoder useResource:primitive usage:MTLResourceUsageRead];[encoder setBuffer:inputs offset:0 atIndex:1];[encoder setBuffer:outputs offset:0 atIndex:2];
      [encoder dispatchThreads:MTLSizeMake(probes.size(),1,1) threadsPerThreadgroup:MTLSizeMake(std::min(NSUInteger(64),pipeline.maxTotalThreadsPerThreadgroup),1,1)];[encoder endEncoding];traceWait(command);
      auto actual=static_cast<const TraceHit*>(outputs.contents);unsigned hits=0;
      // Four changing nearest-hit references per update; full 96-ray/image
      // acceptance is retained at the bounded checkpoints below.
      for(unsigned i=0;i<probes.size();++i){auto& h=actual[i];traceRequire(h.valid<=1,"RT update invalid hit flag");
        if(!h.valid)traceRequire(traceHitMatches(h,TraceHit{}),"RT update invalid miss");
        else {++hits;traceRequire(h.instance==0&&h.primitive<scene.meshIds.size()&&std::isfinite(h.distance)&&h.distance>=probes[i].originMin[3]&&h.distance<=probes[i].directionMax[3]&&std::isfinite(h.barycentric[0])&&std::isfinite(h.barycentric[1])&&h.barycentric[0]>=-.001f&&h.barycentric[1]>=-.001f&&h.barycentric[0]+h.barycentric[1]<=1.001f,"RT update invalid hit");}}
      bool checkpoint=updates==0||updates==29||updates==119;
      for(unsigned n=0;n<(checkpoint?probes.size():4);++n){auto i=checkpoint?n:unsigned((updates*7+n*23)%probes.size());auto expected=traceSceneReference(scene,probes[i]);
        if(const char* reason=traceSceneHitMismatch(scene,probes[i],actual[i],expected)){
          try{saveTraceMismatch(snapshot,probes,actual,i,expected,reason,directory/"mismatch");}
          catch(const std::exception& e){std::fprintf(stderr,"[FO2 RT UPDATE] Mismatch export failed: %s\n",e.what());}
          throw std::runtime_error("RT update CPU/GPU mismatch ("+std::string(reason)+") at frame "+std::to_string(snapshot.frame)+", ray "+std::to_string(i)+", GPU primitive "+std::to_string(actual[i].primitive)+", CPU primitive "+std::to_string(expected.primitive)+", GPU distance "+std::to_string(actual[i].distance)+", CPU distance "+std::to_string(expected.distance));
        }
        if(actual[i].valid&&expected.valid&&traceSceneBarycentricDisagrees(actual[i],traceSceneTriangle(scene,probes[i],actual[i].primitive))){
          ++precisionComparisons;if(precisionComparisons==1)std::printf("[FO2 RT UPDATE] Same-primitive grazing precision equivalence: frame=%llu ray=%u; spatial/range checks passed\n",(unsigned long long)snapshot.frame,i);
        }
      }
      if(!current()){invalidate();return;}
      latestPrimitive=primitive;latestScene=acceleration;retainedAS=buildingAS;buildingAS=0;++updates;
      bool changed=lastFingerprint&&lastFingerprint!=snapshot.fingerprint;lastFingerprint=snapshot.fingerprint;
      auto workerMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
      totalPrepareMs+=snapshot.prepareNs/1e6;maxPrepareMs=std::max(maxPrepareMs,snapshot.prepareNs/1e6);cacheHits+=snapshot.cacheHits;cacheMisses+=snapshot.cacheMisses;
      lastFrame=snapshot.frame;comparisons+=checkpoint?probes.size():4;changedUpdates+=changed;
      maxCaptureMs=std::max(maxCaptureMs,snapshot.collectNs/1e6);maxWorkerMs=std::max(maxWorkerMs,workerMs);totalCaptureMs+=snapshot.collectNs/1e6;totalBuildMs+=buildMs;
      if(updates==1||updates%30==0){report(@{@"status":@"running",@"updates":@(updates),@"source_frame":@(snapshot.frame),@"triangles":@(scene.indices.size()/3),@"vertices":@(scene.positions.size()),@"accepted_draws":@(snapshot.accepted),@"attempts":@(snapshot.attempts),@"limited_draws":@(snapshot.limited),@"unknown_programs":@(snapshot.unknown),@"invalid_draws":@(snapshot.invalid),@"changed_since_previous_update":@(changed),@"capture_cpu_ms":@(snapshot.collectNs/1e6),@"worker_prepare_ms":@(snapshot.prepareNs/1e6),@"mean_worker_prepare_ms":@(totalPrepareMs/updates),@"max_worker_prepare_ms":@(maxPrepareMs),@"mesh_cache_total_hits":@(cacheHits),@"mesh_cache_total_misses":@(cacheMisses),@"mesh_cache_hits":@(snapshot.cacheHits),@"mesh_cache_misses":@(snapshot.cacheMisses),@"mesh_cache_bytes":@(snapshot.cacheBytes),@"mesh_cache_entries":@(snapshot.cacheEntries),@"build_wall_ms":@(buildMs),@"worker_wall_ms":@(workerMs),@"gpu_cached_buffer_bytes":@(buffers()),@"gpu_retained_as_bytes":@(retainedAS),@"gpu_peak_owned_bytes":@(peakBytes),@"gpu_budget_bytes":@(memoryLimit),@"cpu_probes_per_update":@4,@"precision_equivalent_comparisons":@(precisionComparisons),@"cpu_gpu_comparisons":@(comparisons),@"changed_updates":@(changedUpdates),@"max_capture_cpu_ms":@(maxCaptureMs),@"max_worker_core_ms":@(maxWorkerMs),@"mean_capture_cpu_ms":@(totalCaptureMs/updates),@"mean_build_wall_ms":@(totalBuildMs/updates),@"native_rt_gpu_validated":@YES,@"world_space_validated":@NO,@"gameplay_rt_enabled":@NO,@"update_strategy":@"async-full-rebuild-single-flight"});
        std::printf("[FO2 RT UPDATE] updates=%llu frame=%llu triangles=%zu draws=%u limited=%u unknown=%u invalid=%u capture=%.2fms prepare=%.2fms cache=%u/%u build=%.2fms worker=%.2fms GPU-owned=%zu peak=%zu\n",(unsigned long long)updates,(unsigned long long)snapshot.frame,scene.indices.size()/3,snapshot.accepted,snapshot.limited,snapshot.unknown,snapshot.invalid,snapshot.collectNs/1e6,snapshot.prepareNs/1e6,snapshot.cacheHits,snapshot.cacheMisses,buildMs,workerMs,buffers()+retainedAS,peakBytes);}
      if(checkpoint&&current()){
        auto checkpointStart=std::chrono::steady_clock::now();
        auto saved=snapshot;saved.sample=uint32_t(updates);checkLiveTraceSnapshot(saved,0,false);
        auto folder=directory.parent_path()/"rt-live"/("sample-"+std::to_string(updates));NSError* error=nil;
        auto doc=@{@"status":@"passed",@"source_frame":@(snapshot.frame),@"updates":@(updates),@"reference_rays":@(probes.size()),@"reference_mismatches":@0,@"native_rt_gpu_validated":@YES,@"gameplay_rt_enabled":@NO,@"source_kind":@"current-frame-owned-memory"};
        auto data=[NSJSONSerialization dataWithJSONObject:doc options:NSJSONWritingPrettyPrinted error:&error];
        traceRequire(data&&[data writeToFile:[NSString stringWithUTF8String:(folder/"rt-update-check.json").c_str()] options:NSDataWritingAtomic error:&error],"RT update checkpoint report failed");
        lastCheckpointMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-checkpointStart).count();
      }
      if(!current())invalidate();
    }catch(const std::exception& e){
      invalidate();NSMutableDictionary* failure=lastReport?[lastReport mutableCopy]:[NSMutableDictionary dictionary];
      failure[@"status"]=@"failed";failure[@"error"]=[NSString stringWithUTF8String:e.what()];failure[@"updates"]=@(updates);
      failure[@"source_frame"]=@(snapshot.frame);failure[@"last_success_frame"]=@(lastFrame);failure[@"gameplay_rt_enabled"]=@NO;failure[@"native_rt_gpu_validated"]=@NO;
      failure[@"precision_equivalent_comparisons"]=@(precisionComparisons);failure[@"cpu_gpu_comparisons"]=@(comparisons);failure[@"changed_updates"]=@(changedUpdates);
      failure[@"capture_cpu_ms"]=@(snapshot.collectNs/1e6);failure[@"worker_prepare_ms"]=@(snapshot.prepareNs/1e6);
      failure[@"mean_capture_cpu_ms"]=@(updates?totalCaptureMs/updates:0);failure[@"max_capture_cpu_ms"]=@(maxCaptureMs);
      failure[@"mean_worker_prepare_ms"]=@(updates?totalPrepareMs/updates:0);failure[@"max_worker_prepare_ms"]=@(maxPrepareMs);
      failure[@"mesh_cache_total_hits"]=@(cacheHits);failure[@"mesh_cache_total_misses"]=@(cacheMisses);failure[@"gpu_peak_owned_bytes"]=@(peakBytes);
      report(failure);throw;
    }
  }}
};
class TraceUpdates {
  std::shared_ptr<TraceUpdateGPU> gpu=std::make_shared<TraceUpdateGPU>();
  TraceUpdateWorker worker{[context=gpu](const auto& s,const auto& current){context->prepareAndProcess(s,current);},[context=gpu]{context->invalidate();}};
public:
  ~TraceUpdates(){worker.stop();if(!worker.failed())try{gpu->invalidate();gpu->finish();}catch(...){} }
  bool canAccept()const{return worker.canAccept();}bool failed()const{return worker.failed();}
  void invalidate(){worker.invalidate();}
  bool submit(LiveTraceSnapshot&& snapshot){return worker.submit(std::move(snapshot));}
};
}
