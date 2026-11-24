#pragma once
#include "../render/live_trace_scene.h"
namespace fo2render {
inline bool liveTraceEnabled(){const char* v=std::getenv("FO2_RT_LIVE_DIAGNOSTIC");return v&&!std::strcmp(v,"1");}
// Called only for three diagnostic samples, after the main frame was submitted.
// No mutable guest/game buffers survive into this code.
inline int checkLiveTraceSnapshot(const LiveTraceSnapshot& snapshot,uint64_t previousFingerprint,bool runCheck=true,const std::filesystem::path* overrideOutput=nullptr){
  const char* session=std::getenv("FO2_METAL_SESSION_DIR");
  auto output=overrideOutput?*overrideOutput:(session&&*session?std::filesystem::path(session):std::filesystem::path("work/metal"))/"rt-live"/("sample-"+std::to_string(snapshot.sample));
  std::filesystem::create_directories(output);
  auto selected=snapshot.scene.meshIds;std::sort(selected.begin(),selected.end());selected.erase(std::unique(selected.begin(),selected.end()),selected.end());
  NSDictionary* metadata=@{@"schema":@1,@"status":(snapshot.scene.indices.empty()?@"no-mesh":@"cpu-prepared"),@"source_frame":@(snapshot.frame),
    @"coordinate_space":@"right-handed-view",@"coverage":@"submitted-captured-draws-only",
    @"native_rt_gpu_validated":@NO,@"world_space_validated":@NO,
    @"vertices":@(snapshot.scene.positions.size()),@"triangles":@(snapshot.scene.indices.size()/3),
    @"projection":@[@(snapshot.scene.projection[0]),@(snapshot.scene.projection[1]),@(snapshot.scene.projection[2]),@(snapshot.scene.projection[3])],
    @"sample":@(snapshot.sample),@"draws_attempted":@(snapshot.attempts),@"accepted_draws":@(snapshot.accepted),
    @"selected_draws":@(selected.size()),@"capture_cpu_ms":@(snapshot.collectNs/1e6),@"worker_prepare_ms":@(snapshot.prepareNs/1e6),@"mesh_cache_hits":@(snapshot.cacheHits),@"mesh_cache_misses":@(snapshot.cacheMisses),@"mesh_cache_bytes":@(snapshot.cacheBytes),@"update_strategy":(runCheck?@"rebuild-per-diagnostic-sample":@"async-full-rebuild-single-flight"),@"sample_limit":@(runCheck?3:0),@"interval_racing_frames":@(runCheck?120:6),
    @"unknown_programs":@(snapshot.unknown),@"invalid_draws":@(snapshot.invalid),@"limited_draws":@(snapshot.limited),
    @"different_projection":@(snapshot.otherProjection),@"target_limit":@(snapshot.otherTarget),
    @"fingerprint":@(snapshot.fingerprint),@"previous_fingerprint":@(previousFingerprint),
    @"changed_since_previous_sample":@(previousFingerprint&&previousFingerprint!=snapshot.fingerprint),
    @"source_kind":@"current-frame-owned-memory"};
  auto write=[&](const char* name,const void* data,size_t size){std::ofstream f(output/name,std::ios::binary);if(size)f.write(static_cast<const char*>(data),size);traceRequire(bool(f),"Live RT snapshot export failed");};
  NSError* error=nil;auto data=[NSJSONSerialization dataWithJSONObject:metadata options:NSJSONWritingPrettyPrinted error:&error];
  traceRequire(data!=nil,"Live RT snapshot metadata failed");
  write("scene.json",data.bytes,data.length);
  if(snapshot.scene.indices.empty()){std::printf("[FO2 RT LIVE] sample=%u frame=%llu no verified geometry; attempts=%u unknown=%u invalid=%u limited=%u\n",snapshot.sample,(unsigned long long)snapshot.frame,snapshot.attempts,snapshot.unknown,snapshot.invalid,snapshot.limited);return 0;}
  write("positions.f32",snapshot.scene.positions.data(),snapshot.scene.positions.size()*12);
  write("indices.u32",snapshot.scene.indices.data(),snapshot.scene.indices.size()*4);write("triangle-mesh.u32",snapshot.scene.meshIds.data(),snapshot.scene.meshIds.size()*4);
  std::printf("[FO2 RT LIVE] sample=%u frame=%llu triangles=%zu changed=%d attempts=%u unknown=%u invalid=%u limited=%u\n",snapshot.sample,(unsigned long long)snapshot.frame,snapshot.scene.indices.size()/3,int(previousFingerprint&&previousFingerprint!=snapshot.fingerprint),snapshot.attempts,snapshot.unknown,snapshot.invalid,snapshot.limited);
  return runCheck?checkPreparedTraceScene(output.c_str(),true,&snapshot.scene,metadata,&output):0;
}
}
