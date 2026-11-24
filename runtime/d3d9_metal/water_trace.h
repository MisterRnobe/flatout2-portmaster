#pragma once
#include "../render/live_trace_scene.h"
#include "trace_car_shading.h"
#include "trace_car_fixture.h"
namespace fo2render {
inline bool waterTraceEnabled(){const char* s=std::getenv("FO2_RT_WATER");return s&&std::strcmp(s,"1")==0;}
// Screen radiance is gated by the current opaque depth. The verified car-body
// family can instead evaluate its actual textures at the triangle hit, without
// the primary screen's visibility requirement. Unknown materials keep fallback.
inline const char* waterTraceSource=R"MSL(
#include <metal_raytracing>
using namespace raytracing;
struct CarHit {float4 baryConfidence;uint4 identity;};
kernel void waterTrace(instance_acceleration_structure scene [[buffer(0)]],
 constant float4* c [[buffer(1)]], device atomic_uint* counts [[buffer(2)]],
 device CarHit* hits [[buffer(3)]],
 depth2d<float,access::read> opaque [[texture(0)]],
 texture2d<float> radiance [[texture(1)]], texture2d<float> normals [[texture(2)]],
 depth2d<float,access::read> water [[texture(3)]],
 texture2d<float,access::write> output [[texture(4)]],uint2 h [[thread_position_in_grid]]){
 uint2 outSize=uint2(output.get_width(),output.get_height());
 if(any(h>=outSize))return;
 output.write(float4(0),h);
 uint pixel=h.y*outSize.x+h.x;hits[pixel]={float4(0),uint4(0)};
 uint2 size=uint2(opaque.get_width(),opaque.get_height());
 uint2 p=min(uint2((float2(h)+.5)*float2(size)/float2(outSize)),size-1);
 float4 wave=normals.read(p);float z=water.read(p);
 if(wave.a<=.001||!isfinite(z)||z>=.9999999||z>opaque.read(p)+2e-6)return;
 atomic_fetch_add_explicit(counts+0,1u,memory_order_relaxed);
 float3 center=debugPosition(p,water,c),n=wave.rgb*2-1;
 float length2=dot(n,n);
 if(!all(isfinite(center))||!isfinite(length2)||length2<.25)return;
 n*=rsqrt(length2);if(dot(n,center)>0)n=-n;
 float receiver=wave.a*(1-.7*c[3].z)*(1-smoothstep(100.0,150.0,-center.z));
 if(receiver<.001)return;
 float3 incident=normalize(center),direction=reflect(incident,n);
 ray r;r.origin=center+n*.06;r.direction=direction;r.min_distance=.25;r.max_distance=60.25;
 intersector<triangle_data,instancing> query;
 query.assume_geometry_type(geometry_type::triangle);query.accept_any_intersection(false);
 auto hit=query.intersect(r,scene,255u);
 if(hit.type!=intersection_type::triangle)return;
 atomic_fetch_add_explicit(counts+1,1u,memory_order_relaxed);
 float fresnel=c[3].y+(1-c[3].y)*pow(1-clamp(dot(-incident,n),0.0,1.0),5.0);
 float materialConfidence=receiver*(1-hit.distance/61.0)*fresnel;
 if(!isfinite(materialConfidence)||materialConfidence<=0)return;
 hits[pixel]={float4(hit.triangle_barycentric_coord,materialConfidence,0),uint4(hit.primitive_id+1,0,0,0)};
 float3 q=r.origin+direction*hit.distance;
 if(!all(isfinite(q))){hits[pixel].baryConfidence.z=0;return;}
 if(q.z>=-.1){hits[pixel].identity.y=1;atomic_fetch_add_explicit(counts+2,1u,memory_order_relaxed);return;}
 float2 uv=ssrUV(q,c);
 if(any(uv<=.001)||any(uv>=.999)){hits[pixel].identity.y=1;atomic_fetch_add_explicit(counts+2,1u,memory_order_relaxed);return;}
 uint2 hp=min(uint2(uv*float2(size)),size-1);float depth=opaque.read(hp);
 float distance=c[0].w/(depth-c[0].z);
 if(!isfinite(distance)||depth>=.9999999||abs(distance+q.z)>.05+.01*distance||
   (normals.read(hp).a>.001&&water.read(hp)<=depth+2e-6)){
   hits[pixel].identity.y=2;atomic_fetch_add_explicit(counts+3,1u,memory_order_relaxed);return;}
 float edge=min(min(uv.x,1-uv.x),min(uv.y,1-uv.y));
 float confidence=receiver*smoothstep(0.0,.08,edge)*(1-hit.distance/61.0)*fresnel;
 output.write(float4(ssrRoughRadiance(radiance,opaque,uv,c[3].z,c),confidence),h);
 atomic_fetch_add_explicit(counts+4,1u,memory_order_relaxed);
}
)MSL";
// This correctness path is render-thread-owned and synchronous. It never uses
// the asynchronous diagnostic worker's camera-relative AS or mutable buffers.
class WaterTrace {
 id<MTLDevice> device;id<MTLCommandQueue> queue;id<MTLComputePipelineState> pipeline;
 id<MTLTexture> output;uint64_t completed=0;uint32_t lastWaterRays=0;bool fixture=false;double totalMs=0,maxMs=0;
 TraceCarShading carShading;bool carsEnabled=true;
 uint32_t lastMaterialHits=0,lastOffscreenMaterialHits=0,lastRayOnlyMaterialHits=0;
 uint64_t materialHitsTotal=0,offscreenHitsTotal=0,rayOnlyHitsTotal=0,rayOnlyFrames=0;
 id<MTLAccelerationStructure> build(MTLAccelerationStructureDescriptor* descriptor){@autoreleasepool{
   return buildTraceStructure(device,queue,descriptor);
 }}
public:
 uint32_t waterRayCount()const{return lastWaterRays;}
 uint32_t materialHitCount()const{return lastMaterialHits;}
 uint32_t offscreenMaterialHitCount()const{return lastOffscreenMaterialHits;}
 uint32_t rayOnlyMaterialHitCount()const{return lastRayOnlyMaterialHits;}
 WaterTrace(id<MTLDevice> value,bool checking=false):device(value),fixture(checking),carShading(value){@autoreleasepool{
   traceRequire(device&&device.supportsRaytracing,"Water RT unavailable on this device");
   queue=[device newCommandQueue];traceRequire(queue!=nil,"Water RT queue allocation failed");
   auto source=std::string(debugViewSource)+ssrSource+waterTraceSource;
   auto options=[MTLCompileOptions new];options.languageVersion=MTLLanguageVersion2_4;NSError* error=nil;
   auto library=[device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options error:&error];
   if(!library)throw std::runtime_error(std::string("Water RT MSL compile: ")+(error?error.localizedDescription.UTF8String:"unknown"));
   pipeline=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"waterTrace"] error:&error];
   traceRequire(pipeline!=nil,"Water RT pipeline allocation failed");
 }}
 id<MTLTexture> render(LiveTraceSnapshot snapshot,uint64_t displayFrame,const float* constants,
     id<MTLTexture> depth,id<MTLTexture> color,id<MTLTexture> normals,id<MTLTexture> water,
     const TraceTextureResolver& texture={},const TraceSamplerResolver& sampler={}){@autoreleasepool{
   auto start=std::chrono::steady_clock::now();
   traceRequire(snapshot.frame==displayFrame,"Water RT stale display frame");
   finalizeLiveTraceSnapshot(snapshot);const auto& s=snapshot.scene;if(s.indices.empty())return nil;validateTraceScene(s);
   std::array<float,4> projection;std::copy_n(constants,4,projection.begin());
   traceRequire(traceProjectionMatches(s.projection,projection)&&traceRasterShiftMatches(constants[6],constants[7],uint32_t(depth.width),uint32_t(depth.height)),"Water RT camera mismatch");
   traceRequire(depth&&color&&normals&&water&&depth.width==color.width&&depth.height==color.height&&
     normals.width==depth.width&&normals.height==depth.height&&water.width==depth.width&&water.height==depth.height,"Water RT texture dimensions mismatch");
   NSUInteger w=(depth.width+3)/4,h=(depth.height+3)/4;
   traceRequire(w>0&&h>0&&w<=2048&&h<=2048,"Water RT output exceeds limit");
   // All commands using the previous output completed before its replacement.
   if(!output||output.width!=w||output.height!=h){output=nil;
     auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:w height:h mipmapped:NO];
     td.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;td.storageMode=MTLStorageModePrivate;
     output=[device newTextureWithDescriptor:td];traceRequire(output!=nil,"Water RT output allocation failed");}
   size_t vb=s.positions.size()*12,ib=s.indices.size()*4;
   auto vertices=[device newBufferWithBytes:s.positions.data() length:vb options:MTLResourceStorageModeShared];
   auto indices=[device newBufferWithBytes:s.indices.data() length:ib options:MTLResourceStorageModeShared];
   traceRequire(vertices&&indices,"Water RT geometry allocation failed");
   auto geometry=[MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
   geometry.vertexBuffer=vertices;geometry.vertexStride=12;geometry.vertexFormat=MTLAttributeFormatFloat3;
   geometry.indexBuffer=indices;geometry.indexType=MTLIndexTypeUInt32;geometry.triangleCount=s.indices.size()/3;geometry.opaque=YES;
   auto pd=[MTLPrimitiveAccelerationStructureDescriptor descriptor];pd.geometryDescriptors=@[geometry];
   auto ps=[device accelerationStructureSizesWithDescriptor:pd];
   constexpr size_t limit=128*1024*1024;
   size_t hitBytes=w*h*32;
   std::vector<TraceCarGPU> cars;
   if(carsEnabled)try{cars=carShading.prepare(snapshot,texture,sampler);}catch(const std::exception& error){
     if(fixture)throw;carsEnabled=false;std::fprintf(stderr,"[FO2 RT CAR] Disabled material path: %s; screen-radiance RT retained\n",error.what());}
   size_t carBytes=0;for(auto& car:cars)carBytes+=car.range.vertices*(64+96);
   size_t resident=vb+ib+hitBytes+carBytes+output.allocatedSize+sizeof(MTLAccelerationStructureInstanceDescriptor)+36;
   traceRequire(resident<limit&&ps.accelerationStructureSize<limit-resident&&
      ps.buildScratchBufferSize<limit-resident-ps.accelerationStructureSize,"Water RT BLAS memory budget exceeded");
   auto primitive=build(pd);
   MTLAccelerationStructureInstanceDescriptor instance{};
   instance.transformationMatrix.columns[0]=MTLPackedFloat3Make(1,0,0);instance.transformationMatrix.columns[1]=MTLPackedFloat3Make(0,1,0);
   instance.transformationMatrix.columns[2]=MTLPackedFloat3Make(0,0,1);instance.mask=255;
   auto instances=[device newBufferWithBytes:&instance length:sizeof instance options:MTLResourceStorageModeShared];
   traceRequire(instances!=nil,"Water RT instance allocation failed");
   auto td=[MTLInstanceAccelerationStructureDescriptor descriptor];td.instanceCount=1;td.instanceDescriptorBuffer=instances;td.instancedAccelerationStructures=@[primitive];
   auto ts=[device accelerationStructureSizesWithDescriptor:td];resident+=ps.accelerationStructureSize;
   traceRequire(resident<limit&&ts.accelerationStructureSize<limit-resident&&ts.buildScratchBufferSize<limit-resident-ts.accelerationStructureSize,"Water RT TLAS memory budget exceeded");
   auto scene=build(td);
   uint32_t zero[9]{};auto counters=[device newBufferWithBytes:zero length:sizeof zero options:MTLResourceStorageModeShared];
   auto hits=[device newBufferWithLength:hitBytes options:MTLResourceStorageModePrivate];
   traceRequire(counters&&hits,"Water RT hit/counter allocation failed");
   auto command=[queue commandBuffer];auto encoder=[command computeCommandEncoder];traceRequire(encoder!=nil,"Water RT encoder allocation failed");
   [encoder setComputePipelineState:pipeline];[encoder setAccelerationStructure:scene atBufferIndex:0];[encoder useResource:primitive usage:MTLResourceUsageRead];
   [encoder setBytes:constants length:64 atIndex:1];[encoder setBuffer:counters offset:0 atIndex:2];
   [encoder setBuffer:hits offset:0 atIndex:3];
   [encoder setTexture:depth atIndex:0];[encoder setTexture:color atIndex:1];[encoder setTexture:normals atIndex:2];[encoder setTexture:water atIndex:3];[encoder setTexture:output atIndex:4];
   NSUInteger threads=std::min(NSUInteger(64),pipeline.maxTotalThreadsPerThreadgroup);
   [encoder dispatchThreads:MTLSizeMake(w,h,1) threadsPerThreadgroup:MTLSizeMake(threads,1,1)];[encoder endEncoding];
   carShading.encode(command,cars,indices,hits,counters,output);traceWait(command);
   ++completed;auto counts=static_cast<const uint32_t*>(counters.contents);lastWaterRays=counts[0];
   traceRequire(counts[4]<=counts[1]&&counts[1]<=counts[0]&&counts[0]<=w*h,"Water RT invalid counters");
   traceRequire(counts[6]<=counts[5]&&counts[5]<=counts[1],"Water RT invalid material counters");
   traceRequire(counts[7]<=counts[6],"Water RT invalid offscreen material counter");lastMaterialHits=counts[5];lastOffscreenMaterialHits=counts[7];
   traceRequire(counts[8]<=counts[5],"Water RT invalid ray-only material counter");lastRayOnlyMaterialHits=counts[8];
   materialHitsTotal+=counts[5];offscreenHitsTotal+=counts[7];rayOnlyHitsTotal+=counts[8];if(snapshot.rayOnlyDraws)++rayOnlyFrames;
   double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();totalMs+=elapsed;maxMs=std::max(maxMs,elapsed);
   if(!fixture&&(completed==1||completed%300==0)){
     const char* session=std::getenv("FO2_METAL_SESSION_DIR");if(session&&*session){
       auto directory=std::filesystem::path(session)/"rt-water";std::filesystem::create_directories(directory);
       NSDictionary* report=@{@"mode":@"synchronous-hybrid-car-material",@"status":@"running",@"gpu_executed":@YES,
         @"source_frame":@(snapshot.frame),@"display_frame":@(displayFrame),@"completed_frames":@(completed),
         @"full_scene_coverage":@NO,@"offscreen_radiance_supported":@(!cars.empty()),@"car_material_draws":@(cars.size()),
         @"car_material_hits":@(counts[5]),@"car_nonscreen_hits":@(counts[6]),@"car_offscreen_hits":@(counts[7]),@"triangles":@(s.indices.size()/3),@"unknown_draws":@(snapshot.unknown),
         @"ray_only_draws":@(snapshot.rayOnlyDraws),@"ray_only_material_hits":@(counts[8]),
         @"car_material_hits_total":@(materialHitsTotal),@"car_offscreen_hits_total":@(offscreenHitsTotal),
         @"ray_only_material_hits_total":@(rayOnlyHitsTotal),@"frames_with_ray_only_geometry":@(rayOnlyFrames),
         @"width":@(w),@"height":@(h),@"water_rays":@(counts[0]),@"geometry_hits":@(counts[1]),@"offscreen_hits":@(counts[2]),
         @"hidden_radiance_hits":@(counts[3]),@"radiance_hits":@(counts[4]),@"prepare_build_trace_wait_ms":@(elapsed),@"mean_prepare_build_trace_wait_ms":@(totalMs/completed),@"max_prepare_build_trace_wait_ms":@(maxMs)};
       NSError* error=nil;auto data=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:&error];
       traceRequire(data&&[data writeToFile:[NSString stringWithUTF8String:(directory/"progress.json").c_str()] options:NSDataWritingAtomic error:&error],"Water RT report write failed");
     }
   }
   if(completed==1||completed%300==0)std::printf("[FO2 RT WATER] frame=%llu same-frame, triangles=%zu, %lux%lu rays: water=%u hit=%u offscreen=%u hidden=%u radiance=%u; prepare/build/trace/wait %.3f ms (hybrid screen radiance)\n",
      (unsigned long long)displayFrame,s.indices.size()/3,(unsigned long)w,(unsigned long)h,counts[0],counts[1],counts[2],counts[3],counts[4],elapsed);
   if(completed==1||completed%300==0)std::printf("[FO2 RT CAR] frame=%llu materials=%zu shaded=%u non-screen=%u offscreen=%u ray-only-draws=%u ray-only-hits=%u enabled=%d\n",(unsigned long long)displayFrame,cars.size(),counts[5],counts[6],counts[7],snapshot.rayOnlyDraws,counts[8],int(carsEnabled));
   return output;
 }}
};

int checkMetalWaterTrace(){@autoreleasepool{
 auto device=MTLCreateSystemDefaultDevice();if(!device||!device.supportsRaytracing){std::puts("[FO2 RT WATER CHECK] unavailable");return 77;}
 WaterTrace trace(device,true);auto queue=[device newCommandQueue];traceRequire(queue!=nil,"Water fixture queue missing");
 auto texture=[&](MTLPixelFormat format){auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:64 height:64 mipmapped:NO];
   td.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;td.storageMode=format==MTLPixelFormatDepth32Float?MTLStorageModePrivate:MTLStorageModeShared;
   auto image=[device newTextureWithDescriptor:td];traceRequire(image!=nil,"Water fixture texture missing");return image;};
 auto opaque=texture(MTLPixelFormatDepth32Float),water=texture(MTLPixelFormatDepth32Float);
 auto color=texture(MTLPixelFormatRGBA8Unorm),normals=texture(MTLPixelFormatRGBA8Unorm);
 auto clearDepth=[&](id<MTLTexture> image,double z){auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
   pass.depthAttachment.texture=image;pass.depthAttachment.loadAction=MTLLoadActionClear;pass.depthAttachment.storeAction=MTLStoreActionStore;pass.depthAttachment.clearDepth=z;
   auto command=[queue commandBuffer];auto encoder=[command renderCommandEncoderWithDescriptor:pass];traceRequire(encoder!=nil,"Water fixture clear encoder missing");[encoder endEncoding];traceWait(command);};
 LiveTraceSnapshot snapshot;snapshot.frame=42;snapshot.prepared=true;snapshot.scene.projection={1,1,1.01f,-.1f};
 snapshot.scene.positions={{{-100,-100,-4}},{{100,-100,-4}},{{100,100,-4}},{{-100,100,-4}}};
 snapshot.scene.indices={0,1,2,0,2,3};snapshot.scene.meshIds={0,0};
 float c[16]={1,1,1.01f,-.1f,0,0,0,0,32,32,0,1,1,.04f,.1f,3};
 std::vector<uint8_t> pixels(64*64*4),waves(64*64*4);
 for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){auto i=(y*64+x)*4;
   pixels[i]=pixels[i+3]=255;waves[i]=128;waves[i+1]=255;waves[i+2]=128;waves[i+3]=y>=32?255:0;}
 [color replaceRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:256];
 auto upload=[&]{[normals replaceRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0 withBytes:waves.data() bytesPerRow:256];};upload();
 clearDepth(opaque,c[2]+c[3]/4);clearDepth(water,c[2]+c[3]/2);
 auto read=[&](id<MTLTexture> image){auto buffer=[device newBufferWithLength:256*image.height options:MTLResourceStorageModeShared];traceRequire(buffer!=nil,"Water fixture readback buffer missing");
   auto command=[queue commandBuffer];auto encoder=[command blitCommandEncoder];traceRequire(encoder!=nil,"Water fixture readback encoder missing");
   [encoder copyFromTexture:image sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(image.width,image.height,1)
     toBuffer:buffer destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256*image.height];[encoder endEncoding];traceWait(command);
   std::vector<uint16_t> result(image.width*image.height*4);
   for(NSUInteger y=0;y<image.height;++y)std::memcpy(result.data()+y*image.width*4,static_cast<const uint8_t*>(buffer.contents)+y*256,image.width*8);return result;};
 auto render=[&]{return read(trace.render(snapshot,42,c,opaque,color,normals,water));};
 auto image=render();unsigned hits=0;
 for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x){auto i=(y*16+x)*4;
   if(y<8)traceRequire(image[i+3]==0,"Water RT modified non-water pixels");
   if(image[i+3]){++hits;traceRequire(image[i]==0x3c00&&image[i+1]==0&&image[i+2]==0,"Water RT wrong reflected radiance");}}
 traceRequire(hits>20,"Water RT synthetic wall reflection missing");
 c[6]=1.f/64;c[7]=-1.f/64;auto biased=render();unsigned biasedHits=0;
 for(size_t i=3;i<biased.size();i+=4)if(biased[i])++biasedHits;
 traceRequire(biasedHits>20,"Water RT rejected D3D9 half-pixel raster bias");
 for(unsigned y=32;y<64;++y)for(unsigned x=0;x<64;++x)waves[(y*64+x)*4+3]=64;upload();
 auto partial=render();unsigned partialHits=0;
 for(size_t i=3;i<partial.size();i+=4)if(partial[i]){++partialHits;traceRequire(biased[i]>partial[i],"Water RT ignored authored partial coverage");}
 traceRequire(partialHits>20,"Water RT rejected low-coverage physical water");

 // The actual triangle, independent double-precision CPU intersection.
 TraceRay ray;ray.originMin={0,-.5f,-2,.25f};ray.directionMax={0,.25f,-1,60.25f};ray.mask={255,0,0,0};
 traceRequire(traceSceneReference(snapshot.scene,ray).valid==1,"Water RT independent wall fixture miss");
 auto empty=[&](const std::vector<uint16_t>& data){for(size_t i=3;i<data.size();i+=4)traceRequire(data[i]==0,"Water RT emitted hidden/range-rejected radiance");};
 clearDepth(opaque,c[2]+c[3]);empty(render()); // Foreground occludes water.
 clearDepth(opaque,c[2]+c[3]/4);
 for(size_t i=3;i<waves.size();i+=4)waves[i]=255;upload();empty(render()); // Reflected wall hidden by water.
 for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)waves[(y*64+x)*4+3]=y>=32?255:0;upload();
 // An independently colored car material must supply blue radiance despite
 // the red screen, including hits which project beyond the screen edge.
 auto carDraw=traceCarFixtureDraw();
 auto set=[&](Draw& draw,bool vertex,unsigned reg,std::array<float,4> value){size_t at=0;auto shader=vertex?draw.vs:draw.ps;auto& bytes=vertex?draw.vsConstants:draw.psConstants;
   for(auto u:shader->uniforms)if(u.kind==0){if(reg>=u.index&&reg<u.index+u.count){std::memcpy(bytes.data()+at+(reg-u.index)*16,value.data(),16);return;}at+=u.count*16;}
   throw std::runtime_error("Car fixture missing constant");};
 set(carDraw,true,0,{1,0,0,0});set(carDraw,true,1,{0,1,0,0});set(carDraw,true,2,{0,0,-c[2],c[3]});set(carDraw,true,3,{0,0,-1,0});
 set(carDraw,true,4,{1,0,0,0});set(carDraw,true,5,{0,1,0,0});set(carDraw,true,6,{0,0,1,0});set(carDraw,true,15,{0,0,1,0});
 for(unsigned r:{17,18,19})set(carDraw,true,r,{0,1,0,0});set(carDraw,false,1,{1,1,1,1});
 auto material=std::make_shared<TraceCarMaterial>();material->vs=carDraw.vs;material->ps=carDraw.ps;
 material->vsConstants=carDraw.vsConstants;material->psConstants=carDraw.psConstants;
 material->effects[62]=1;
 for(auto p:snapshot.scene.positions)material->inputs.push_back({p[0],p[1],p[2],1,0,1,0,1,0,0,0,0,.5f,.5f,0,1});
 std::array<id<MTLTexture>,4> carTextures;std::array<id<MTLSamplerState>,4> carSamplers;
 auto sd=[MTLSamplerDescriptor new];sd.minFilter=sd.magFilter=MTLSamplerMinMagFilterNearest;
 for(unsigned i=0;i<4;++i){auto im=std::make_shared<Image>();im->faces=i%2?6:1;im->rgba.resize(im->faces,Bytes{0,0,uint8_t(i%2?0:255),0});
   material->textures[i]=im;material->versions[i]=im->version;
   auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
   td.textureType=i%2?MTLTextureTypeCube:MTLTextureType2D;td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageShaderRead;
   carTextures[i]=[device newTextureWithDescriptor:td];carSamplers[i]=[device newSamplerStateWithDescriptor:sd];traceRequire(carTextures[i]&&carSamplers[i],"Car fixture texture allocation");
   for(unsigned f=0;f<im->faces;++f)[carTextures[i] replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 slice:f withBytes:im->rgba[f].data() bytesPerRow:4 bytesPerImage:4];
 }
 TraceTextureResolver resolve=[&](const std::shared_ptr<Image>& im,bool){for(unsigned i=0;i<4;++i)if(material->textures[i]==im)return carTextures[i];return (id<MTLTexture>)nil;};
 TraceSamplerResolver sampler=[&](const Sampler&){return carSamplers[0];};
 snapshot.cars={{material,0,2,0,4}};
 clearDepth(opaque,c[2]+c[3]/3); // Hides reflected wall, but leaves water visible.
 for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){auto i=(y*64+x)*4;waves[i]=230;waves[i+1]=128;waves[i+2]=204;waves[i+3]=y>=32?255:0;}upload();
 auto shaded=read(trace.render(snapshot,42,c,opaque,color,normals,water,resolve,sampler));
 unsigned blue=0;for(size_t i=0;i<shaded.size();i+=4)if(shaded[i+3]){++blue;traceRequire(shaded[i]==0&&shaded[i+1]==0&&shaded[i+2]==0x3c00,"RT car color came from screen instead of blue material");}
 traceRequire(blue>20&&trace.materialHitCount()==blue&&trace.offscreenMaterialHitCount()>0,"RT car offscreen material shading missing");
 traceRequire(trace.rayOnlyMaterialHitCount()==blue,"RT car lost ray-only hit provenance");
 ++material->textures[0]->version;empty(read(trace.render(snapshot,42,c,opaque,color,normals,water,resolve,sampler)));
 traceRequire(trace.materialHitCount()==0,"RT car reused a changed texture version");snapshot.cars.clear();
 for(auto& p:snapshot.scene.positions)p[2]=-100;empty(render()); // Out of finite ray range.
 bool staleRejected=false;try{trace.render(snapshot,43,c,opaque,color,normals,water);}catch(const std::exception&){staleRejected=true;}
 traceRequire(staleRejected,"Water RT accepted stale frame");
 std::puts("[FO2 RT WATER CHECK] passed: reflected wall, independent CPU ray, non-water, foreground/water occlusion, finite range, D3D9 half-pixel bias, stale-frame rejection and offscreen textured car material independent of screen radiance");return 0;
}}
}
