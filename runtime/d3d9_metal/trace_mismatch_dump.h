#pragma once
namespace fo2render {
// Preserve the exact compared inputs once, before the failed updater is disabled.
inline void saveTraceMismatch(const LiveTraceSnapshot& snapshot,const std::vector<TraceRay>& rays,
                              const TraceHit* hits,unsigned index,const TraceHit& expected,
                              const char* reason,const std::filesystem::path& folder){
  auto saved=snapshot;checkLiveTraceSnapshot(saved,0,false,&folder);
  auto write=[&](const char* name,const void* data,size_t bytes){std::ofstream f(folder/name,std::ios::binary);f.write(static_cast<const char*>(data),bytes);traceRequire(bool(f),"RT mismatch binary write failed");};
  write("rays.bin",rays.data(),rays.size()*sizeof(TraceRay));write("hits.bin",hits,rays.size()*sizeof(TraceHit));
  const auto& actual=hits[index];const auto& ray=rays[index];
  auto number=[](float value)->id{return std::isfinite(value)?id(@(value)):id([NSNull null]);};
  auto hit=[&](const TraceHit& h){return @{@"valid":@(h.valid),@"primitive":@(h.primitive),@"instance":@(h.instance),@"distance":number(h.distance),@"u":number(h.barycentric[0]),@"v":number(h.barycentric[1])};};
  auto triangle=[&](const TraceHit& h){NSMutableArray* result=[NSMutableArray array];
    if(h.valid&&h.primitive<snapshot.scene.indices.size()/3)for(unsigned k=0;k<3;++k){auto p=snapshot.scene.positions[snapshot.scene.indices[h.primitive*3+k]];[result addObject:@[@(p[0]),@(p[1]),@(p[2])]];}return result;};
  TraceHit own;if(actual.valid&&actual.primitive<snapshot.scene.indices.size()/3)own=traceSceneTriangle(snapshot.scene,ray,actual.primitive);
  auto doc=@{@"schema":@1,@"source_frame":@(snapshot.frame),@"ray_index":@(index),@"ray_count":@(rays.size()),@"ray_stride":@(sizeof(TraceRay)),@"hit_stride":@(sizeof(TraceHit)),@"byte_order":@"little-endian",
    @"reason":[NSString stringWithUTF8String:reason],@"origin_min":@[@(ray.originMin[0]),@(ray.originMin[1]),@(ray.originMin[2]),@(ray.originMin[3])],
    @"direction_max":@[@(ray.directionMax[0]),@(ray.directionMax[1]),@(ray.directionMax[2]),@(ray.directionMax[3])],@"mask":@(ray.mask[0]),
    @"actual":hit(actual),@"expected_nearest":hit(expected),@"exact_cpu_on_reported_primitive":hit(own),
    @"actual_triangle":triangle(actual),@"expected_triangle":triangle(expected),
    @"distance_tolerance":@(.001f+.0001f*std::abs(expected.distance)),@"barycentric_tolerance":@(.002f),@"gameplay_rt_enabled":@NO};
  NSError* error=nil;auto data=[NSJSONSerialization dataWithJSONObject:doc options:NSJSONWritingPrettyPrinted error:&error];
  traceRequire(data&&[data writeToFile:[NSString stringWithUTF8String:(folder/"mismatch.json").c_str()] options:NSDataWritingAtomic error:&error],"RT mismatch JSON write failed");
  std::fprintf(stderr,"[FO2 RT UPDATE] Exact mismatch scene/rays/hits saved: %s\n",folder.c_str());
}
int replayTraceMismatch(const char* directory){@autoreleasepool{
  traceRequire(directory&&*directory,"RT replay directory required");std::filesystem::path folder=directory;
  NSDictionary* metadata=nil;auto scene=loadTraceScene(folder,&metadata);
  auto rayBytes=traceSceneFile(folder/"rays.bin",512*sizeof(TraceRay)),hitBytes=traceSceneFile(folder/"hits.bin",512*sizeof(TraceHit));
  traceRequire(rayBytes.size()%sizeof(TraceRay)==0&&hitBytes.size()%sizeof(TraceHit)==0&&rayBytes.size()/sizeof(TraceRay)==hitBytes.size()/sizeof(TraceHit),"RT replay ABI/count mismatch");
  std::vector<TraceRay> rays(rayBytes.size()/sizeof(TraceRay));std::vector<TraceHit> hits(rays.size());
  std::memcpy(rays.data(),rayBytes.data(),rayBytes.size());std::memcpy(hits.data(),hitBytes.data(),hitBytes.size());
  unsigned mismatches=0,precision=0;
  for(unsigned i=0;i<rays.size();++i){auto expected=traceSceneReference(scene,rays[i]);
    if(const char* reason=traceSceneHitMismatch(scene,rays[i],hits[i],expected)){++mismatches;std::printf("[FO2 RT REPLAY] ray=%u mismatch=%s\n",i,reason);}
    else if(hits[i].valid&&traceSceneBarycentricDisagrees(hits[i],traceSceneTriangle(scene,rays[i],hits[i].primitive)))++precision;
  }
  auto report=@{@"status":mismatches?@"failed":@"passed",@"source_frame":metadata[@"source_frame"],@"rays":@(rays.size()),@"mismatches":@(mismatches),@"precision_equivalent_comparisons":@(precision),@"gpu_executed":@NO,@"mode":@"recorded-gpu-results-cpu-replay"};
  NSError* error=nil;auto data=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:&error];
  traceRequire(data&&[data writeToFile:[NSString stringWithUTF8String:(folder/"replay-report.json").c_str()] options:NSDataWritingAtomic error:&error],"RT replay report write failed");
  std::printf("[FO2 RT REPLAY] %zu recorded GPU rays: %u mismatches, %u spatially equivalent grazing comparisons\n",rays.size(),mismatches,precision);
  return mismatches?1:0;
}}
}
