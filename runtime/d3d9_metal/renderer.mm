#include "../d3d9_gles/vkbd.h"
#include "../d3d9_gles/vkbd_font.h"
#include "../render/commands.h"
#include "../render/state_keys.h"
#include "../render/trace_capture.h"
#include "../render/shader_capture.h"
#include "../render/validate.h"
#include "debug_views.h"
#include "ssr.h"
#include "../render/material_shader.h"
#include "../render/water.h"
#include "../render/water_replacement.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_metal.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <dispatch/dispatch.h>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

#include "raytracing_check.h"
#include "trace_scene_check.h"
#include "live_trace_check.h"
#include "trace_updates.h"
#include "water_trace.h"
#include "physical_water_bridge.h"

static std::atomic<bool> debugRaceActive{false},debugViewReady{false},waterViewReady{false};
void fo2MetalSetRaceActive(bool active){debugRaceActive.store(active);if(!active){debugViewReady=false;waterViewReady=false;}}
bool fo2MetalRaceActive(){return debugRaceActive.load();}
bool fo2MetalDebugReady(){return debugViewReady.load();}
bool fo2MetalWaterReady(){return waterViewReady.load();}
void fo2MetalSetDebugReady(bool ready){debugViewReady=ready;}
static std::atomic<bool> ssrEnabled{true},waterTraceRequested{true};
static std::atomic<int> waterTraceState{1};
void fo2MetalToggleWaterTrace(){if(!fo2render::waterTraceEnabled())return;waterTraceRequested=!waterTraceRequested.load();std::printf("[FO2 RT WATER] F7: %s\n",waterTraceRequested.load()?"on":"off (SSR only)");}
const char* fo2MetalWaterTraceLabel(){if(!fo2render::waterTraceEnabled())return "";
  if(waterTraceState.load()==3)return " | RT WATER FAILED (SSR)";
  if(!waterTraceRequested.load())return " | RT WATER OFF [F7]";
  return waterTraceState.load()==2?" | RT WATER ACTIVE [F7]":" | RT WATER WAITING [F7]";}

void fo2MetalToggleSSR(){ssrEnabled=!ssrEnabled.load();std::printf("[FO2 SSR] F8: %s\n",ssrEnabled.load()?"on":"off");}
bool fo2MetalSSREnabled(){return ssrEnabled.load();}
static SDL_MetalView metalView = nullptr;
void fo2MetalPrepareWindow(void *window) {
  if (metalView)
    SDL_Metal_DestroyView(metalView);
  metalView = SDL_Metal_CreateView(static_cast<SDL_Window *>(window));
  if (!metalView)
    throw std::runtime_error(std::string("SDL Metal view: ") + SDL_GetError());
}
void fo2MetalDestroyWindow() {
  if (metalView) {
    SDL_Metal_DestroyView(metalView);
    metalView = nullptr;
  }
}
namespace fo2render {
namespace {
std::filesystem::path traceCaptureDirectory(){
  const char* enabled=std::getenv("FO2_RT_GEOMETRY_CAPTURE");
  if(!enabled||std::strcmp(enabled,"1"))return {};
  const char* session=std::getenv("FO2_METAL_SESSION_DIR");
  return (session&&*session?std::filesystem::path(session):std::filesystem::path("work/metal"))/"rt-geometry";
}
struct GPUBuffer {
  id<MTLBuffer> value;
  uint64_t version = 0;
};
struct GPUImage {
  id<MTLTexture> value, srgb;
  uint64_t version = 0;
};
struct GPUShader {
  id<MTLLibrary> library;
  id<MTLFunction> function;
};
static std::runtime_error error(const char *what, NSError *e = nil) {
  return std::runtime_error(
      std::string(what) +
      (e ? ": " + std::string(e.localizedDescription.UTF8String) : ""));
}
MTLCompareFunction compare(Compare c) {
  return static_cast<MTLCompareFunction>(static_cast<unsigned>(c));
}
MTLBlendFactor blend(Blend b) {
  switch (b) {
  case Blend::Zero:
    return MTLBlendFactorZero;
  case Blend::One:
    return MTLBlendFactorOne;
  case Blend::SrcColor:
    return MTLBlendFactorSourceColor;
  case Blend::InvSrcColor:
    return MTLBlendFactorOneMinusSourceColor;
  case Blend::SrcAlpha:
    return MTLBlendFactorSourceAlpha;
  case Blend::InvSrcAlpha:
    return MTLBlendFactorOneMinusSourceAlpha;
  case Blend::DstAlpha:
    return MTLBlendFactorDestinationAlpha;
  case Blend::InvDstAlpha:
    return MTLBlendFactorOneMinusDestinationAlpha;
  case Blend::DstColor:
    return MTLBlendFactorDestinationColor;
  case Blend::InvDstColor:
    return MTLBlendFactorOneMinusDestinationColor;
  case Blend::SrcAlphaSat:
    return MTLBlendFactorSourceAlphaSaturated;
  case Blend::Constant:
    return MTLBlendFactorBlendColor;
  case Blend::InvConstant:
    return MTLBlendFactorOneMinusBlendColor;
  }
  return MTLBlendFactorOne;
}
MTLVertexFormat format(Element t) {
  switch (t) {
  case Element::Float1:
    return MTLVertexFormatFloat;
  case Element::Float2:
    return MTLVertexFormatFloat2;
  case Element::Float3:
    return MTLVertexFormatFloat3;
  case Element::Float4:
    return MTLVertexFormatFloat4;
  case Element::Color:
  case Element::Byte4N:
    return MTLVertexFormatUChar4Normalized;
  case Element::Byte4:
    return MTLVertexFormatUChar4;
  case Element::Short2:
    return MTLVertexFormatShort2;
  case Element::Short4:
    return MTLVertexFormatShort4;
  case Element::Short2N:
    return MTLVertexFormatShort2Normalized;
  case Element::Short4N:
    return MTLVertexFormatShort4Normalized;
  case Element::UShort2N:
    return MTLVertexFormatUShort2Normalized;
  case Element::UShort4N:
    return MTLVertexFormatUShort4Normalized;
  case Element::Half2:
    return MTLVertexFormatHalf2;
  case Element::Half4:
    return MTLVertexFormatHalf4;
  }
  return MTLVertexFormatInvalid;
}
MTLSamplerAddressMode address(uint32_t mode) {
  switch (mode) {
  case 1:
    return MTLSamplerAddressModeRepeat;
  case 2:
    return MTLSamplerAddressModeMirrorRepeat;
  case 3:
    return MTLSamplerAddressModeClampToEdge;
  case 4:
    return MTLSamplerAddressModeClampToBorderColor;
  case 5:
    return MTLSamplerAddressModeMirrorClampToEdge;
  default:
    return MTLSamplerAddressModeClampToEdge;
  }
}
const char *blitSource = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Out {float4 position [[position]];float2 uv;};
vertex Out blitVS(uint id [[vertex_id]]){float2 p=float2((id==1)?3.0:-1.0,(id==2)?3.0:-1.0);return {float4(p,0,1),float2((p.x+1)*0.5,1-(p.y+1)*0.5)};}
fragment float4 blitPS(Out p [[stage_in]],texture2d<float> image [[texture(0)]],constant float4& rect [[buffer(0)]],sampler smp [[sampler(0)]]){return image.sample(smp,rect.xy+p.uv*rect.zw);}
)MSL";
} // namespace
struct Renderer::Impl {
  struct Metrics {
    std::atomic<uint64_t> gpuNs{0}, completed{0}, failed{0};
  };
  std::shared_ptr<Metrics> metrics = std::make_shared<Metrics>();
  std::shared_ptr<Image> font;
  uint64_t draws = 0, encodeNs = 0;
  std::chrono::steady_clock::time_point lastReport =
      std::chrono::steady_clock::now();
  id<MTLDevice> device;
  id<MTLCommandQueue> queue;
  id<MTLCommandBuffer> command;
  id<MTLRenderCommandEncoder> encoder;
  id<MTLTexture> passColor, passDepth, passMaterial;
  bool passStencil = false;
  std::shared_ptr<Image> null2D, nullCube;
  uint64_t passes = 0;
  CAMetalLayer *layer = nil;
  std::unordered_map<std::string, id<MTLRenderPipelineState>> pipelines;
  std::unordered_map<uint32_t, id<MTLRenderPipelineState>> blitPipelines;
  std::unordered_map<std::string, id<MTLDepthStencilState>> depths;
  std::unordered_map<std::string, id<MTLSamplerState>> samplers;
  id<MTLLibrary> blitLibrary;
  dispatch_semaphore_t slots = dispatch_semaphore_create(2);
  uint64_t frames = 0;
  unsigned reflectionInspections=0;
  std::shared_ptr<Shader> inspectionDepthPS;
  PhysicalWaterBridge physicalWater{waterTraceEnabled()};
  TraceCapture traceCapture{traceCaptureDirectory()};
  LiveTraceScene liveTrace{liveTraceEnabled()&&!waterTraceEnabled(),5,0,{},120,true};
  bool rtWaterOn=waterTraceEnabled();
  LiveTraceScene waterTraceScene{waterTraceEnabled(),1,0,{},1,true,true};
  std::unique_ptr<WaterTrace> waterTrace;
  std::optional<LiveTraceSnapshot> selectedWaterTrace;
  std::shared_ptr<Image> currentWaterTrace;
  uint64_t currentWaterTraceFrame=0,rtInspectionFrame=0;
  unsigned rtAutoInspections=0;
  bool rayOnlyInspected=false;
  bool detachedInspected=false;
  std::shared_ptr<Shader> inspectionReflectionPS;
  // Do not create native threads during guest startup/video initialization.
  // The first fully collected racing scene creates the worker lazily.
  std::unique_ptr<TraceUpdates> traceUpdates;
  bool traceWasRacing=false;
  uint32_t width = 1280, height = 800;
  bool debugEnabled=false,debugValid=false,debugCaptureColor=false,debugCaptureMaterials=false;
  std::shared_ptr<Image> debugSource,debugDepth,debugOutput,debugColorSource,debugScene,ssrHalf,materialLive,materialSaved;
  std::shared_ptr<Shader> debugVS,debugPS,ssrPS,ssrCompositePS;
  std::shared_ptr<Image> lateScene,lateSceneColorSource,lateSceneDepthSource;
  bool lateSceneReady=false;
  std::unordered_map<std::string,std::shared_ptr<Shader>> materialShaders;
  std::array<uint64_t,5> materialDraws{};
  struct WaterLayer {bool active=false;std::shared_ptr<Image> sourceColor,sourceDepth,normals,depth,color,legacy;};
  std::array<WaterLayer,4> waterLayers;
  std::unordered_map<std::string,std::shared_ptr<Shader>> legacyWaterShaders;
  std::shared_ptr<Shader> legacyWaterShader(const std::shared_ptr<Shader>& original,bool replacement){
    auto key=pipelineShaderIdentity(*original)+(replacement?":replace-water":":zero-water");
    auto found=legacyWaterShaders.find(key);if(found!=legacyWaterShaders.end())return found->second;
    if(legacyWaterShaders.size()>=32)throw std::runtime_error("Water replacement shader budget exceeded");
    auto shader=std::make_shared<Shader>(*original);shader->gpu.reset();shader->materialVariant.reset();shader->waterVariant.reset();
    shader->source=replacement?waterLegacyWindowSource(original->source):waterLegacyZeroSource(original->source);
    shader->hash=shaderHash(shader->source.data(),shader->source.size());return legacyWaterShaders.emplace(key,shader).first->second;
  }
  std::shared_ptr<Image> selectedColorSource,selectedDepthSource;
  uint64_t waterDraws=0,waterDropped=0;
  std::shared_ptr<Shader> materialShader(const std::shared_ptr<Shader>& original){
    if(original->materialVariant)return original->materialVariant;
    auto key=pipelineShaderIdentity(*original);key+=original->terrainSpecularMask?":terrain":":uniform";auto found=materialShaders.find(key);
    if(found!=materialShaders.end()){original->materialVariant=found->second;return found->second;}
    auto shader=std::make_shared<Shader>(*original);shader->gpu.reset();
    shader->source=materialOutputSource(original->source,original->terrainSpecularMask);shader->hash=shaderHash(shader->source.data(),shader->source.size());
    original->materialVariant=materialShaders.emplace(key,shader).first->second;return original->materialVariant;
  }
  bool sceneDraw(const Draw& d)const{
    return debugEnabled&&d.debugProjection.valid&&d.depth&&d.raster.depthOn&&d.raster.depthWrite&&
       d.color->width==width&&d.color->height==height&&d.depth->width==width&&d.depth->height==height&&
       d.raster.viewport[0]==0&&d.raster.viewport[1]==0&&d.raster.viewport[2]==width&&d.raster.viewport[3]==height;
  }
  bool materialSceneDraw(const Draw& d)const{
    const bool same=debugSource==d.depth&&debugColorSource==d.color;
    return d.captureMaterial&&debugCaptureMaterials&&debugEnabled&&d.depth&&d.raster.depthOn&&
      d.color->width==width&&d.color->height==height&&d.depth->width==width&&d.depth->height==height&&
      d.raster.viewport[0]==0&&d.raster.viewport[1]==0&&d.raster.viewport[2]==width&&d.raster.viewport[3]==height&&
      ((d.raster.depthWrite&&(d.debugProjection.valid||same))||(!d.raster.depthWrite&&same&&d.material.cue!=MaterialCue::Unknown));
  }
  std::shared_ptr<Image> colorImage(){auto out=std::make_shared<Image>();out->width=width;out->height=height;out->rgba={Bytes(size_t(width)*height*4)};return out;}
  DebugProjection pendingProjection,selectedProjection;
  std::array<float,2> pendingShift{},selectedShift{};
  uint64_t pendingWeight=0,bestWeight=0,projectionWeight=0;
  void resetDebugFrame(){currentWaterTrace.reset();selectedWaterTrace.reset();lateSceneReady=false;lateSceneColorSource.reset();lateSceneDepthSource.reset();for(auto& layer:waterLayers){layer.active=false;layer.sourceColor.reset();layer.sourceDepth.reset();}selectedColorSource.reset();selectedDepthSource.reset();debugSource.reset();debugColorSource.reset();pendingWeight=bestWeight=projectionWeight=0;debugValid=false;}
  void saveDebugDepth(){
    if(debugSource&&pendingWeight>bestWeight){
      if(!debugDepth||debugDepth->width!=debugSource->width||debugDepth->height!=debugSource->height||
         debugDepth->stencil!=debugSource->stencil){
        debugDepth=std::make_shared<Image>();debugDepth->width=debugSource->width;
        debugDepth->height=debugSource->height;debugDepth->depth=true;debugDepth->stencil=debugSource->stencil;
      }
      endPass();begin();
      auto source=image(debugSource),target=image(debugDepth);
      auto blit=[command blitCommandEncoder];
      [blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                 sourceSize:MTLSizeMake(debugSource->width,debugSource->height,1)
                  toTexture:target destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
      if(debugCaptureColor&&debugColorSource){
        if(!debugScene||debugScene->width!=width||debugScene->height!=height){
          debugScene=std::make_shared<Image>();debugScene->width=width;debugScene->height=height;
          debugScene->rgba={Bytes(size_t(width)*height*4)};
        }
        [blit copyFromTexture:image(debugColorSource) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                   sourceSize:MTLSizeMake(width,height,1) toTexture:image(debugScene)
             destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
      }
      if(debugCaptureMaterials&&materialLive){
        if(!materialSaved||materialSaved->width!=width||materialSaved->height!=height)materialSaved=colorImage();
        [blit copyFromTexture:image(materialLive) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                   sourceSize:MTLSizeMake(width,height,1) toTexture:image(materialSaved)
             destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
      }
      [blit endEncoding];
      bestWeight=pendingWeight;selectedColorSource=debugColorSource;selectedDepthSource=debugSource;selectedProjection=pendingProjection;selectedShift=pendingShift;debugValid=true;
      selectedWaterTrace=rtWaterOn&&waterTraceRequested.load()?waterTraceScene.copyDisplayFrame(frames,selectedColorSource,selectedDepthSource,selectedProjection.values):std::nullopt;
    }
    debugSource.reset();debugColorSource.reset();pendingWeight=projectionWeight=0;
  }
  void noteDebugDraw(const Draw& d){
    if(!sceneDraw(d))return;
    if(debugSource!=d.depth || (debugCaptureColor&&debugColorSource!=d.color)){saveDebugDepth();debugSource=d.depth;debugColorSource=d.color;}
    pendingWeight+=uint64_t(d.count)*d.instanceCount;
    if(d.count>projectionWeight){projectionWeight=d.count;pendingProjection=d.debugProjection;
      pendingShift={d.effects[20],d.effects[21]};}
  }

  struct UniformPage {
    id<MTLBuffer> buffer;
    size_t used = 0;
  };
  struct UniformPool {
    std::mutex mutex;
    std::vector<std::shared_ptr<UniformPage>> free;
  };
  struct UniformSlice {
    id<MTLBuffer> buffer;
    NSUInteger offset;
  };
  std::shared_ptr<UniformPool> uniformPool = std::make_shared<UniformPool>();
  std::vector<std::shared_ptr<UniformPage>> uniformPages;
  UniformSlice uniforms(const void *bytes, size_t size) {
    begin();
    const size_t aligned = (size + 255) & ~size_t(255);
    if (uniformPages.empty() || uniformPages.back()->used + aligned >
                                    uniformPages.back()->buffer.length) {
      std::shared_ptr<UniformPage> page;
      {
        std::lock_guard<std::mutex> lock(uniformPool->mutex);
        auto &free = uniformPool->free;
        auto found = std::find_if(free.begin(), free.end(), [&](const auto &p) {
          return p->buffer.length >= aligned;
        });
        if (found != free.end()) {
          page = *found;
          free.erase(found);
        }
      }
      if (!page) {
        page = std::make_shared<UniformPage>();
        page->buffer =
            [device newBufferWithLength:std::max<size_t>(256 * 1024, aligned)
                                options:MTLResourceStorageModeShared];
        if (!page->buffer)
          throw error("Metal uniform page allocation");
        page->buffer.label = @"FlatOut 2 frame uniforms";
      }
      page->used = 0;
      uniformPages.push_back(page);
    }
    auto &page = uniformPages.back();
    auto offset = page->used;
    std::memcpy(static_cast<uint8_t *>(page->buffer.contents) + offset, bytes,
                size);
    page->used += aligned;
    return {page->buffer, offset};
  }
  Impl() {
    device = MTLCreateSystemDefaultDevice();
    if (!device)
      throw error("Metal device unavailable");
    queue = [device newCommandQueue];
    if (!queue)
      throw error("Metal command queue unavailable");
    NSError *e = nil;
    MTLCompileOptions *options = [MTLCompileOptions new];
    options.fastMathEnabled = NO;
    options.languageVersion = MTLLanguageVersion3_0;
    blitLibrary =
        [device newLibraryWithSource:[NSString stringWithUTF8String:blitSource]
                             options:options
                               error:&e];
    if (!blitLibrary)
      throw error("Metal blit shader", e);
    std::printf("[Metal] device: %s\n", device.name.UTF8String);
    const char *api = std::getenv("MTL_DEBUG_LAYER"),
               *gpu = std::getenv("MTL_SHADER_VALIDATION");
    std::printf("[Metal] validation environment: API=%s GPU=%s\n",
                api ? api : "unset", gpu ? gpu : "unset");
  }
  void begin() {
    if (metrics->failed.load())
      throw error(
          "A previous Metal GPU submission failed; see GPU validation log");
    if (!command) {
      dispatch_semaphore_wait(slots, DISPATCH_TIME_FOREVER);
      command = [queue commandBuffer];
      command.label = @"FlatOut 2 frame";
    }
  }
  void endPass() {
    if (encoder) {
      [encoder endEncoding];
      encoder = nil;
      passColor = nil;
      passDepth = nil;
      passMaterial = nil;
    }
  }
  std::shared_ptr<Image> nullImage(bool cube) {
    auto &image = cube ? nullCube : null2D;
    if (!image) {
      image = std::make_shared<Image>();
      image->faces = cube ? 6 : 1;
      image->rgba.resize(image->faces, Bytes{0, 0, 0, 255});
    }
    return image;
  }
  void submit(bool wait) {
    if (!command)
      return;
    endPass();
    id<MTLCommandBuffer> submitted = command;
    auto sem = slots;
    auto stats = metrics;
    auto pages = std::move(uniformPages);
    uniformPages.clear();
    auto pool = uniformPool;
    [submitted addCompletedHandler:^(id<MTLCommandBuffer> b) {
      if (b.status == MTLCommandBufferStatusError) {
        stats->failed.fetch_add(1);
        fprintf(stderr, "[Metal] GPU error: %s\n",
                b.error.localizedDescription.UTF8String);
      }
      if (b.GPUEndTime > b.GPUStartTime)
        stats->gpuNs.fetch_add(uint64_t((b.GPUEndTime - b.GPUStartTime) * 1e9));
      stats->completed.fetch_add(1);
      {
        std::lock_guard<std::mutex> lock(pool->mutex);
        for (auto &page : pages)
          pool->free.push_back(page);
      }
      dispatch_semaphore_signal(sem);
    }];
    [submitted commit];
    command = nil;
    if (wait) {
      [submitted waitUntilCompleted];
      if (submitted.status == MTLCommandBufferStatusError)
        throw error("Metal command failed", submitted.error);
    }
  }
  id<MTLBuffer> buffer(const std::shared_ptr<Buffer> &b) {
    if (!b)
      return nil;
    auto p = std::static_pointer_cast<GPUBuffer>(b->gpu);
    if (!p) {
      p = std::make_shared<GPUBuffer>();
      b->gpu = p;
    }
    if (p->version != b->version) {
      p->value = [device newBufferWithBytes:b->bytes.data()
                                     length:std::max<size_t>(b->bytes.size(), 1)
                                    options:MTLResourceStorageModeShared];
      if (!p->value)
        throw error("Metal buffer allocation");
      p->version = b->version;
    }
    return p->value;
  }
  id<MTLTexture> image(const std::shared_ptr<Image> &image) {
    if (!image)
      return nil;
    auto p = std::static_pointer_cast<GPUImage>(image->gpu);
    if (!p) {
      p = std::make_shared<GPUImage>();
      image->gpu = p;
    }
    if (image->parent) {
      auto parent = this->image(image->parent);
      if (p->version != image->parent->version) {
        p->value = [parent
            newTextureViewWithPixelFormat:parent.pixelFormat
                              textureType:MTLTextureType2D
                                   levels:NSMakeRange(image->baseLevel, 1)
                                   slices:NSMakeRange(image->baseFace, 1)];
        if (!p->value)
          throw error("Metal texture view allocation");
        p->srgb = nil;
        p->version = image->parent->version;
      }
      return p->value;
    }
    if (p->version != image->version) {
      MTLTextureDescriptor *d = [MTLTextureDescriptor new];
      d.textureType = image->faces == 6 ? MTLTextureTypeCube : MTLTextureType2D;
      d.width = image->width;
      d.height = image->height;
      d.mipmapLevelCount = image->levels;
      d.pixelFormat =
          image->depth ? (image->stencil ? MTLPixelFormatDepth32Float_Stencil8
                                         : MTLPixelFormatDepth32Float)
                       : MTLPixelFormatRGBA8Unorm;
      d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget |
                MTLTextureUsagePixelFormatView;
      d.storageMode =
          image->depth ? MTLStorageModePrivate : MTLStorageModeShared;
      p->value = [device newTextureWithDescriptor:d];
      if (!p->value)
        throw error("Metal texture allocation");
      if (!image->depth)
        for (uint32_t face = 0; face < image->faces; ++face)
          for (uint32_t mip = 0; mip < image->levels; ++mip) {
            size_t i = face * image->levels + mip;
            uint32_t w = std::max(1u, image->width >> mip),
                     h = std::max(1u, image->height >> mip);
            if (i < image->rgba.size() &&
                image->rgba[i].size() >= size_t(w) * h * 4)
              [p->value replaceRegion:MTLRegionMake2D(0, 0, w, h)
                          mipmapLevel:mip
                                slice:face
                            withBytes:image->rgba[i].data()
                          bytesPerRow:w * 4
                        bytesPerImage:w * h * 4];
          }
      p->srgb = nil;
      p->version = image->version;
    }
    return p->value;
  }
  id<MTLTexture> formatImage(const std::shared_ptr<Image> &value, bool srgb) {
    auto base = image(value);
    if (!base || !srgb || value->depth)
      return base;
    auto p = std::static_pointer_cast<GPUImage>(value->gpu);
    if (!p->srgb)
      p->srgb =
          [base newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB];
    if (!p->srgb)
      throw error("Metal sRGB view creation");
    return p->srgb;
  }
  id<MTLFunction> shader(const std::shared_ptr<Shader> &s) {
    if (!s)
      throw error("Missing Metal shader");
    auto p = std::static_pointer_cast<GPUShader>(s->gpu);
    if (!p) {
      p = std::make_shared<GPUShader>();
      NSError *e = nil;
      MTLCompileOptions *options = [MTLCompileOptions new];
      options.fastMathEnabled = NO;
      options.languageVersion = MTLLanguageVersion3_0;
      p->library = [device
          newLibraryWithSource:[NSString stringWithUTF8String:s->source.c_str()]
                       options:options
                         error:&e];
      if (!p->library) {
        std::error_code ec;
        std::filesystem::create_directories("work/metal/failures", ec);
        std::ofstream("work/metal/failures/" + std::to_string(s->hash) +
                      ".metal")
            << s->source;
        throw error("MSL compile failed", e);
      }
      p->function = [p->library
          newFunctionWithName:[NSString stringWithUTF8String:s->entry.c_str()]];
      if (!p->function)
        throw error("Metal entry point missing");
      s->gpu = p;
    }
    return p->function;
  }
  id<MTLSamplerState> sampler(const Sampler &s) {
    auto key = samplerStateKey(s);
    auto i = samplers.find(key);
    if (i != samplers.end())
      return i->second;
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.sAddressMode = address(s.address[0]);
    d.tAddressMode = address(s.address[1]);
    d.rAddressMode = address(s.address[2]);
    d.minFilter = s.minLinear ? MTLSamplerMinMagFilterLinear
                              : MTLSamplerMinMagFilterNearest;
    d.magFilter = s.magLinear ? MTLSamplerMinMagFilterLinear
                              : MTLSamplerMinMagFilterNearest;
    d.mipFilter =
        s.mipLinear ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
    d.lodMinClamp = s.minLod;
    d.lodMaxClamp = s.mipEnabled ? 1000.0f : s.minLod;
    d.maxAnisotropy = std::max(1u, std::min(16u, uint32_t(s.maxAnisotropy)));
    return samplers.emplace(key, [device newSamplerStateWithDescriptor:d])
        .first->second;
  }
  id<MTLDepthStencilState> depth(const Raster &r) {
    auto key = depthStateKey(r);
    auto i = depths.find(key);
    if (i != depths.end())
      return i->second;
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    d.depthCompareFunction =
        r.depthOn ? compare(r.depthCompare) : MTLCompareFunctionAlways;
    d.depthWriteEnabled = r.depthOn && r.depthWrite;
    if (r.stencilOn) {
      MTLStencilDescriptor *s = [MTLStencilDescriptor new];
      s.stencilCompareFunction = compare(r.stencilCompare);
      s.stencilFailureOperation =
          static_cast<MTLStencilOperation>(r.stencilFail);
      s.depthFailureOperation =
          static_cast<MTLStencilOperation>(r.stencilDepthFail);
      s.depthStencilPassOperation =
          static_cast<MTLStencilOperation>(r.stencilPass);
      s.readMask = r.stencilRead;
      s.writeMask = r.stencilWrite;
      d.frontFaceStencil = s;
      if (r.twoSidedStencil) {
        MTLStencilDescriptor *back = [s copy];
        back.stencilCompareFunction = compare(r.backStencilCompare);
        back.stencilFailureOperation =
            static_cast<MTLStencilOperation>(r.backStencilFail);
        back.depthFailureOperation =
            static_cast<MTLStencilOperation>(r.backStencilDepthFail);
        back.depthStencilPassOperation =
            static_cast<MTLStencilOperation>(r.backStencilPass);
        d.backFaceStencil = back;
      } else
        d.backFaceStencil = s;
    }
    return depths.emplace(key, [device newDepthStencilStateWithDescriptor:d])
        .first->second;
  }
  id<MTLRenderPipelineState> pipeline(const Draw &draw,const std::shared_ptr<Shader>& pixelShader,const std::shared_ptr<Image>& materialTarget) {
    auto color = formatImage(draw.color, draw.colorSrgb),
         depth = image(draw.depth);
    auto key = pipelineStateKey(draw,*pixelShader,uint32_t(color.pixelFormat),
                                depth?uint32_t(depth.pixelFormat):0,
                                materialTarget?uint32_t(image(materialTarget).pixelFormat):0);
    auto i = pipelines.find(key);
    if (i != pipelines.end())
      return i->second;
    MTLRenderPipelineDescriptor *p = [MTLRenderPipelineDescriptor new];
    auto pipelineStart=std::chrono::steady_clock::now();
    p.vertexFunction = shader(draw.vs);
    p.fragmentFunction = shader(pixelShader);
    auto a = p.colorAttachments[0];
    a.pixelFormat = color.pixelFormat;
    a.blendingEnabled = draw.raster.blendOn;
    a.sourceRGBBlendFactor = blend(draw.raster.src);
    a.destinationRGBBlendFactor = blend(draw.raster.dst);
    a.sourceAlphaBlendFactor = blend(draw.raster.srcAlpha);
    a.destinationAlphaBlendFactor = blend(draw.raster.dstAlpha);
    a.rgbBlendOperation = static_cast<MTLBlendOperation>(draw.raster.blend);
    a.alphaBlendOperation =
        static_cast<MTLBlendOperation>(draw.raster.blendAlpha);
    a.writeMask = static_cast<MTLColorWriteMask>(
        ((draw.raster.colorMask & 1) ? MTLColorWriteMaskRed : 0) |
        ((draw.raster.colorMask & 2) ? MTLColorWriteMaskGreen : 0) |
        ((draw.raster.colorMask & 4) ? MTLColorWriteMaskBlue : 0) |
        ((draw.raster.colorMask & 8) ? MTLColorWriteMaskAlpha : 0));
    if (depth) {
      p.depthAttachmentPixelFormat = depth.pixelFormat;
      if (draw.depth->stencil)
        p.stencilAttachmentPixelFormat = depth.pixelFormat;
    }
    MTLVertexDescriptor *v = [MTLVertexDescriptor vertexDescriptor];
    for (auto x : draw.attributes) {
      auto at = v.attributes[x.slot];
      at.format = format(x.type);
      at.offset = x.offset;
      at.bufferIndex = x.stream + 1;
      v.layouts[x.stream + 1].stride = draw.strides[x.stream];
      v.layouts[x.stream + 1].stepFunction =
          (draw.perInstanceMask & (1u << x.stream))
              ? MTLVertexStepFunctionPerInstance
              : MTLVertexStepFunctionPerVertex;
      v.layouts[x.stream + 1].stepRate = std::max(1u, draw.stepRates[x.stream]);
      if (!draw.strides[x.stream]) {
        v.layouts[x.stream + 1].stepFunction = MTLVertexStepFunctionConstant;
        v.layouts[x.stream + 1].stepRate = 0;
      }
    }
    p.vertexDescriptor = v;
    if(materialTarget){p.colorAttachments[1].pixelFormat=image(materialTarget).pixelFormat;
      p.colorAttachments[1].blendingEnabled=NO;p.colorAttachments[1].writeMask=MTLColorWriteMaskAll;}
    NSError *e = nil;
    auto state = [device newRenderPipelineStateWithDescriptor:p error:&e];
    if (!state) {
      std::error_code ec;
      std::filesystem::create_directories("work/metal/failures", ec);
      auto name =
          std::to_string(draw.vs->hash) + "-" + std::to_string(pixelShader->hash);
      std::ofstream("work/metal/failures/" + name + "-vs.metal")
          << draw.vs->source;
      std::ofstream("work/metal/failures/" + name + "-ps.metal")
          << pixelShader->source;
      std::ofstream("work/metal/failures/" + name + ".txt")
          << "binary state key (hex): " << stateKeyHex(key) << "\n"
          << e.localizedDescription.UTF8String;
      throw error("Metal pipeline", e);
    }
    double pipelineMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-pipelineStart).count();
    if(pipelineMs>5)std::printf("[FO2 PIPELINE] frame=%llu new state %.2fms vs=%llu ps=%llu total=%zu\n",(unsigned long long)frames,pipelineMs,(unsigned long long)draw.vs->hash,(unsigned long long)pixelShader->hash,pipelines.size()+1);
    return pipelines.emplace(key, state).first->second;
  }
  id<MTLRenderCommandEncoder> pass(id<MTLTexture> color, id<MTLTexture> depth,
                                   bool stencil, id<MTLTexture> material=nil) {
    begin();
    if (encoder && passColor == color && passDepth == depth &&
        passStencil == stencil && passMaterial == material)
      return encoder;
    endPass();
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = color;
    p.colorAttachments[0].loadAction = MTLLoadActionLoad;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    if(material){p.colorAttachments[1].texture=material;p.colorAttachments[1].loadAction=MTLLoadActionLoad;
      p.colorAttachments[1].storeAction=MTLStoreActionStore;}
    if (depth) {
      p.depthAttachment.texture = depth;
      p.depthAttachment.loadAction = MTLLoadActionLoad;
      p.depthAttachment.storeAction = MTLStoreActionStore;
      if (stencil) {
        p.stencilAttachment.texture = depth;
        p.stencilAttachment.loadAction = MTLLoadActionLoad;
        p.stencilAttachment.storeAction = MTLStoreActionStore;
      }
    }
    encoder = [command renderCommandEncoderWithDescriptor:p];
    passColor = color;
    passMaterial = material;
    passDepth = depth;
    passStencil = stencil;
    ++passes;
    return encoder;
  }
  void blit(id<MTLTexture> source, id<MTLTexture> target,
            const uint32_t *sr = nullptr, const uint32_t *dr = nullptr,
            bool linear = true) {
    endPass();
    begin();
    uint32_t key = uint32_t(target.pixelFormat);
    auto found = blitPipelines.find(key);
    id<MTLRenderPipelineState> pipeline = nil;
    if (found != blitPipelines.end())
      pipeline = found->second;
    else {
      MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
      d.vertexFunction = [blitLibrary newFunctionWithName:@"blitVS"];
      d.fragmentFunction = [blitLibrary newFunctionWithName:@"blitPS"];
      d.colorAttachments[0].pixelFormat = target.pixelFormat;
      NSError *e = nil;
      pipeline = [device newRenderPipelineStateWithDescriptor:d error:&e];
      if (!pipeline)
        throw error("Blit pipeline", e);
      blitPipelines[key] = pipeline;
    }
    auto encoder = pass(target, nil, false);
    [encoder setRenderPipelineState:pipeline];
    [encoder setFragmentTexture:source atIndex:0];
    float uv[4] = {0, 0, 1, 1};
    if (sr) {
      uv[0] = float(sr[0]) / source.width;
      uv[1] = float(sr[1]) / source.height;
      uv[2] = float(sr[2]) / source.width;
      uv[3] = float(sr[3]) / source.height;
    }
    auto uvSlice = uniforms(uv, sizeof(uv));
    [encoder setFragmentBuffer:uvSlice.buffer offset:uvSlice.offset atIndex:0];
    Sampler sm;
    sm.address[0] = sm.address[1] = 3;
    sm.minLinear = sm.magLinear = linear;
    [encoder setFragmentSamplerState:sampler(sm) atIndex:0];
    if (dr)
      [encoder setViewport:MTLViewport{double(dr[0]), double(dr[1]),
                                       double(dr[2]), double(dr[3]), 0, 1}];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:3];
    endPass();
  }
};
Renderer::Renderer() : impl(std::make_unique<Impl>()) {}
Renderer::~Renderer() {
  try {
    wait();
  } catch (...) {
  }
}
void Renderer::attach(void *window) {
  if (window) {
    if (!metalView)
      throw error("Metal view not prepared on main thread");
    impl->layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(metalView);
    impl->layer.device = impl->device;
    impl->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    impl->layer.framebufferOnly = YES;
    impl->layer.maximumDrawableCount = 2;
  }
}
void Renderer::resize(uint32_t w, uint32_t h) {
  impl->waterTraceScene.reset();impl->waterTrace.reset();
  impl->liveTrace.reset();if(impl->traceUpdates)impl->traceUpdates->invalidate();
  impl->width = w;
  impl->height = h;
  impl->resetDebugFrame();impl->debugDepth.reset();impl->debugOutput.reset();
}
std::string Renderer::deviceName() const {
  return impl->device.name.UTF8String;
}
uint64_t Renderer::frames() const { return impl->frames; }
void Renderer::clear(const std::shared_ptr<Image> &color,
                     const std::shared_ptr<Image> &depth, uint32_t flags,
                     const float *rgba, float z, uint32_t stencil,
                     const uint32_t *rect) {
  if(impl->debugEnabled&&(((flags&2)&&impl->debugSource==depth)||((flags&1)&&impl->debugCaptureColor&&impl->debugColorSource==color)))impl->saveDebugDepth();
  // Preserve geometry together with the selected depth/color snapshot before
  // discarding the live target's old segment, including partial clears.
  impl->waterTraceScene.discardDisplayTarget((flags&1)?color:nullptr,(flags&2)?depth:nullptr);
  if (rect) {
    Draw d;
    d.color = color;
    d.depth = depth;
    d.count = 3;
    d.raster.depthOn = bool(flags & 2);
    d.raster.depthWrite = bool(flags & 2);
    d.raster.depthCompare = Compare::Always;
    d.raster.colorMask = (flags & 1) ? 15 : 0;
    d.raster.stencilOn = depth && depth->stencil && (flags & 4);
    d.raster.stencilPass = StencilOp::Replace;
    d.raster.stencilRef = stencil;
    d.raster.scissorOn = true;
    std::copy_n(rect, 4, d.raster.scissor);
    d.raster.viewport[2] = color ? color->width : depth->width;
    d.raster.viewport[3] = color ? color->height : depth->height;
    d.vs = std::make_shared<Shader>();
    d.vs->vertex = true;
    d.vs->entry = "clearVS";
    d.vs->source = R"MSL(#include <metal_stdlib>
using namespace metal;
struct O{float4 p [[position]];};vertex O clearVS(uint i [[vertex_id]]){O o;o.p=float4(i==1?3.0:-1.0,i==2?3.0:-1.0,0,1);return o;}
)MSL";
    d.ps = std::make_shared<Shader>();
    d.ps->entry = "clearPS";
    d.ps->source = "#include <metal_stdlib>\nusing namespace metal;\nstruct "
                   "O{float4 c [[color(0)]];";
    if (depth)
      d.ps->source += "float z [[depth(any)]];";
    d.ps->source +=
        "};fragment O clearPS(constant float4* c [[buffer(0)]]){O o;o.c=c[0];";
    if (depth)
      d.ps->source += "o.z=c[1].x;";
    d.ps->source += "return o;}";
    for (auto s : {d.vs, d.ps})
      s->hash = shaderHash(s->source.data(), s->source.size());
    d.psConstants.resize(32);
    std::memcpy(d.psConstants.data(), rgba, 16);
    std::memcpy(d.psConstants.data() + 16, &z, 4);
    draw(d);
    return;
  }
  @autoreleasepool {
    impl->endPass();
    impl->begin();
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    if (color) {
      auto a = p.colorAttachments[0];
      a.texture = impl->image(color);
      a.loadAction = flags & 1 ? MTLLoadActionClear : MTLLoadActionLoad;
      a.storeAction = MTLStoreActionStore;
      a.clearColor = MTLClearColorMake(rgba[0], rgba[1], rgba[2], rgba[3]);
    }
    if (depth) {
      p.depthAttachment.texture = impl->image(depth);
      p.depthAttachment.loadAction =
          flags & 2 ? MTLLoadActionClear : MTLLoadActionLoad;
      p.depthAttachment.storeAction = MTLStoreActionStore;
      p.depthAttachment.clearDepth = z;
      if (depth->stencil) {
        p.stencilAttachment.texture = impl->image(depth);
        p.stencilAttachment.loadAction =
            flags & 4 ? MTLLoadActionClear : MTLLoadActionLoad;
        p.stencilAttachment.storeAction = MTLStoreActionStore;
        p.stencilAttachment.clearStencil = stencil;
      }
    }
    auto e = [impl->command renderCommandEncoderWithDescriptor:p];
    [e endEncoding];
  }
}
void Renderer::validateShader(const std::shared_ptr<Shader> &s) {
  @autoreleasepool {
    impl->shader(s);
  }
}
void Renderer::draw(const Draw &input) {
  if(input.rayOnly){
    try{
      if(impl->waterTraceScene.collecting()&&fo2MetalRaceActive()&&input.captureMaterial&&impl->sceneDraw(input)&&
         !input.raster.blendOn&&!input.effects[0]&&!input.raster.fill){validateDrawBuffers(input);impl->waterTraceScene.record(input);}
    }catch(const std::exception& e){static unsigned logged=0;if(logged++<8)std::fprintf(stderr,"[FO2 RT POPULATION] Extra draw rejected: %s; ordinary rendering retained\n",e.what());}
    return; // No color/depth/material attachment, water replay or normal draw.
  }
  auto started = std::chrono::steady_clock::now();
  std::array<float,3> physicalNormal{};std::shared_ptr<Buffer> physicalIndices;bool physical=false;
  @autoreleasepool {
    physical=fo2MetalRaceActive()&&impl->debugEnabled&&impl->debugCaptureMaterials&&input.color&&input.depth&&
      input.color->width==impl->width&&input.color->height==impl->height&&input.depth->width==impl->width&&input.depth->height==impl->height&&
      input.raster.viewport[0]==0&&input.raster.viewport[1]==0&&input.raster.viewport[2]==impl->width&&input.raster.viewport[3]==impl->height&&impl->physicalWater.match(input,physicalNormal,physicalIndices);
    const Draw& d=input;
    auto pixelShader=d.ps;auto materialTarget=d.materialTarget;
    std::array<float,64> materialEffects;
    const float* effectValues=d.effects.data();
    if(!input.color||!input.count)return;
    // Capture completed scene color before the first qualified native HUD draw.
    // Earlier water/opaque snapshots can precede lightmaps and postprocessing.
    if(d.hudBoundary&&!impl->lateSceneReady&&impl->debugEnabled&&impl->debugCaptureColor){
      impl->saveDebugDepth();
      if(impl->debugValid&&impl->selectedColorSource==d.color&&d.color->width==impl->width&&d.color->height==impl->height&&
         d.raster.viewport[0]==0&&d.raster.viewport[1]==0&&d.raster.viewport[2]==impl->width&&d.raster.viewport[3]==impl->height){
        if(!impl->lateScene||impl->lateScene->width!=impl->width||impl->lateScene->height!=impl->height)impl->lateScene=impl->colorImage();
        impl->endPass();impl->begin();auto blit=[impl->command blitCommandEncoder];
        [blit copyFromTexture:impl->image(d.color) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                  sourceSize:MTLSizeMake(impl->width,impl->height,1) toTexture:impl->image(impl->lateScene)
            destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];[blit endEncoding];
        impl->lateSceneColorSource=impl->selectedColorSource;impl->lateSceneDepthSource=impl->selectedDepthSource;impl->lateSceneReady=true;
      }
    }
    // Preserve the previous segment before replacing any material attachment.
    if(impl->materialSceneDraw(input)){
      ++impl->materialDraws[unsigned(input.material.cue)];
      if(impl->debugSource&&(impl->debugSource!=input.depth||impl->debugColorSource!=input.color))impl->saveDebugDepth();
      if(!impl->materialLive||impl->materialLive->width!=impl->width||impl->materialLive->height!=impl->height)impl->materialLive=impl->colorImage();
      if(!impl->debugSource){float zero[4]{};clear(impl->materialLive,{},1,zero,1,0);}
      materialTarget=impl->materialLive;pixelShader=impl->materialShader(input.ps);
      materialEffects=input.effects;materialEffects[24]=input.material.reflectivity;
      materialEffects[25]=input.material.roughness;materialEffects[26]=float(unsigned(input.material.cue))/255;
      effectValues=materialEffects.data();
    }
    validateDrawBuffers(d);
    if(impl->traceCapture.enabled()&&fo2MetalRaceActive()&&d.captureMaterial&&impl->sceneDraw(d)&&
       !d.raster.blendOn&&!d.effects[0]&&!d.raster.fill)
      impl->traceCapture.record(d);
    if(impl->liveTrace.collecting()&&fo2MetalRaceActive()&&d.captureMaterial&&impl->sceneDraw(d)&&
       !d.raster.blendOn&&!d.effects[0]&&!d.raster.fill)
      impl->liveTrace.record(d);
    if(impl->waterTraceScene.collecting()&&fo2MetalRaceActive()&&d.captureMaterial&&impl->sceneDraw(d)&&
       !d.raster.blendOn&&!d.effects[0]&&!d.raster.fill)
      impl->waterTraceScene.record(d);
    if(impl->debugEnabled&&impl->debugSource&&
       ((d.debugProjection.valid&&(impl->debugSource!=d.depth || (impl->debugCaptureColor&&impl->debugColorSource!=d.color))) ||
        (impl->debugCaptureColor&&impl->debugColorSource==d.color&&!d.debugProjection.valid&&(!d.raster.depthOn||!d.raster.depthWrite)&&!(d.captureMaterial&&d.raster.depthOn&&d.material.cue!=MaterialCue::Unknown))))
      impl->saveDebugDepth();
    auto pipeline = impl->pipeline(d,pixelShader,materialTarget);
    auto e = impl->pass(impl->formatImage(d.color, d.colorSrgb),
                        impl->image(d.depth), d.depth && d.depth->stencil,impl->image(materialTarget));
    [e setRenderPipelineState:pipeline];
    [e setDepthStencilState:impl->depth(d.raster)];
    [e setStencilReferenceValue:d.raster.stencilRef];
    auto &r = d.raster;
    [e setViewport:MTLViewport{r.viewport[0], r.viewport[1], r.viewport[2],
                               r.viewport[3], r.viewport[4], r.viewport[5]}];
    if (r.scissorOn) {
      NSUInteger x = std::min<uint32_t>(r.scissor[0], d.color->width),
                 y = std::min<uint32_t>(r.scissor[1], d.color->height);
      [e setScissorRect:MTLScissorRect{x, y,
                                       std::min<NSUInteger>(r.scissor[2],
                                                            d.color->width - x),
                                       std::min<NSUInteger>(
                                           r.scissor[3], d.color->height - y)}];
    } else
      [e setScissorRect:MTLScissorRect{0, 0, d.color->width, d.color->height}];
    [e setFrontFacingWinding:MTLWindingClockwise];
    [e setCullMode:r.cull == 1   ? MTLCullModeFront
                   : r.cull == 2 ? MTLCullModeBack
                                 : MTLCullModeNone];
    [e setTriangleFillMode:r.fill ? MTLTriangleFillModeLines
                                  : MTLTriangleFillModeFill];
    [e setBlendColorRed:r.blendColor[0]
                  green:r.blendColor[1]
                   blue:r.blendColor[2]
                  alpha:r.blendColor[3]];
    [e setDepthBias:r.depthBias slopeScale:r.slopeBias clamp:0];
    auto bindConstants = [&](const Bytes &bytes, bool vertex) {
      if (bytes.empty())
        return;
      auto slice = impl->uniforms(bytes.data(), bytes.size());
      if (vertex)
        [e setVertexBuffer:slice.buffer offset:slice.offset atIndex:0];
      else
        [e setFragmentBuffer:slice.buffer offset:slice.offset atIndex:0];
    };
    bindConstants(d.vsConstants, true);
    bindConstants(d.psConstants, false);
    auto effects = impl->uniforms(effectValues, sizeof(d.effects));
    [e setVertexBuffer:effects.buffer offset:effects.offset atIndex:30];
    [e setFragmentBuffer:effects.buffer offset:effects.offset atIndex:30];
    uint32_t streams = 0;
    for (auto &a : d.attributes)
      streams |= 1u << a.stream;
    for (unsigned i = 0; i < 16; ++i) {
      if (streams & (1u << i)) {
        if (d.streams[i]->transient) {
          auto slice = impl->uniforms(d.streams[i]->bytes.data(),
                                      d.streams[i]->bytes.size());
          [e setVertexBuffer:slice.buffer
                      offset:slice.offset + d.offsets[i]
                     atIndex:i + 1];
        } else
          [e setVertexBuffer:impl->buffer(d.streams[i])
                      offset:d.offsets[i]
                     atIndex:i + 1];
      }
      if (pixelShader->samplerMask & (1u << i)) {
        auto texture =
            d.textures[i]
                ? d.textures[i]
                : impl->nullImage(bool(pixelShader->cubeSamplerMask & (1u << i)));
        [e setFragmentTexture:impl->formatImage(
                                  texture, bool(d.textureSrgbMask & (1u << i)))
                      atIndex:i];
        [e setFragmentSamplerState:impl->sampler(d.samplers[i]) atIndex:i];
      }
    }
    MTLPrimitiveType primitive =
        d.primitive == Primitive::Points      ? MTLPrimitiveTypePoint
        : d.primitive == Primitive::Lines     ? MTLPrimitiveTypeLine
        : d.primitive == Primitive::LineStrip ? MTLPrimitiveTypeLineStrip
        : d.primitive == Primitive::Triangles ? MTLPrimitiveTypeTriangle
                                              : MTLPrimitiveTypeTriangleStrip;
    if (d.indices)
      [e drawIndexedPrimitives:primitive
                    indexCount:d.count
                     indexType:d.indexSize == 4 ? MTLIndexTypeUInt32
                                                : MTLIndexTypeUInt16
                   indexBuffer:impl->buffer(d.indices)
             indexBufferOffset:size_t(d.indexStart) * d.indexSize
                 instanceCount:d.instanceCount
                    baseVertex:d.baseVertex
                  baseInstance:0];
    else
      [e drawPrimitives:primitive
            vertexStart:d.vertexStart
            vertexCount:d.count
          instanceCount:d.instanceCount];
  }
  impl->noteDebugDraw(input);
  // Replay verified pool surfaces and physics-backed static wet triangles
  // into private depth/normal images; original game draws remain untouched.
  try{if(impl->debugEnabled&&impl->debugCaptureMaterials&&input.captureMaterial&&
      ((input.waterBasisValid&&input.material.cue==MaterialCue::WorldWater&&input.ps->waterVariant)||physical)&&input.depth&&
      input.color->width==impl->width&&input.color->height==impl->height&&
      input.depth->width==impl->width&&input.depth->height==impl->height&&
      input.raster.viewport[0]==0&&input.raster.viewport[1]==0&&input.raster.viewport[2]==impl->width&&input.raster.viewport[3]==impl->height){
    Impl::WaterLayer* selected=nullptr;
    for(auto& layer:impl->waterLayers)if(layer.active&&layer.sourceColor==input.color&&layer.sourceDepth==input.depth){selected=&layer;break;}
    if(!selected)for(auto& layer:impl->waterLayers)if(!layer.active){selected=&layer;break;}
    if(selected){auto& layer=*selected;
      if(!layer.active){
        if(!layer.normals||layer.normals->width!=impl->width||layer.normals->height!=impl->height){layer.normals=impl->colorImage();layer.color=impl->colorImage();layer.depth.reset();}
        if(!layer.depth||layer.depth->stencil!=input.depth->stencil){layer.depth=std::make_shared<Image>();layer.depth->width=impl->width;layer.depth->height=impl->height;layer.depth->depth=true;layer.depth->stencil=input.depth->stencil;}
        float zero[4]{};clear(layer.normals,{},1,zero,1,0);
        if(waterReplacementEnabled()){
          if(!layer.legacy||layer.legacy->width!=impl->width||layer.legacy->height!=impl->height)layer.legacy=impl->colorImage();
          clear(layer.legacy,{},1,zero,1,0);
        }
        impl->endPass();impl->begin();auto blit=[impl->command blitCommandEncoder];
        [blit copyFromTexture:impl->image(input.depth) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                  sourceSize:MTLSizeMake(impl->width,impl->height,1) toTexture:impl->image(layer.depth)
            destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];[blit endEncoding];
        layer.active=true;layer.sourceColor=input.color;layer.sourceDepth=input.depth;
      }
      Draw surface=input;surface.color=layer.normals;surface.depth=layer.depth;
      if(physical){
        surface.indices=physicalIndices;surface.indexSize=4;surface.indexStart=0;surface.baseVertex=0;surface.vertexStart=0;
        surface.primitive=Primitive::Triangles;surface.count=uint32_t(physicalIndices->bytes.size()/4);
        if(!input.ps->waterVariant){auto shader=std::make_shared<Shader>(*input.ps);shader->gpu.reset();shader->materialVariant.reset();shader->waterVariant.reset();
          shader->source=physicalWaterNormalSource(input.ps->source,input.ps->terrainSpecularMask);shader->hash=shaderHash(shader->source.data(),shader->source.size());input.ps->waterVariant=shader;}
        surface.ps=input.ps->waterVariant;
      }else surface.ps=input.ps->waterVariant;
      surface.colorSrgb=false;surface.materialTarget.reset();surface.captureMaterial=false;surface.waterBasisValid=false;surface.debugProjection={};surface.material={};
      if(waterReplacementEnabled()&&layer.legacy){
        bool replace=physical&&input.ps->physicalWaterFamily==1&&!input.raster.blendOn&&!input.effects[0]&&!input.raster.fill&&!input.colorSrgb;
        surface.ps=impl->legacyWaterShader(replace?input.ps:surface.ps,replace);surface.materialTarget=layer.legacy;
      }
      surface.raster.depthWrite=true;if(physical)surface.raster.depthCompare=Compare::LessEqual;surface.raster.blendOn=false;surface.raster.colorMask=15;surface.raster.stencilOn=false;
      if(physical)std::copy(physicalNormal.begin(),physicalNormal.end(),surface.effects.begin()+48);
      else std::copy(input.waterBasis.begin(),input.waterBasis.end(),surface.effects.begin()+48);draw(surface);
      impl->endPass();impl->begin();auto blit=[impl->command blitCommandEncoder];
      [blit copyFromTexture:impl->image(input.color) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                sourceSize:MTLSizeMake(impl->width,impl->height,1) toTexture:impl->image(layer.color)
          destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];[blit endEncoding];
      ++impl->waterDraws;
    }else ++impl->waterDropped;
  }
  }catch(const std::exception& e){
    if(!physical)throw;
    impl->physicalWater.disable();
    for(auto& layer:impl->waterLayers)if(layer.sourceColor==input.color&&layer.sourceDepth==input.depth)layer.active=false;
    std::fprintf(stderr,"[FO2 PHYSICAL WATER] capture disabled: %s; original rendering/SSR retained\n",e.what());
  }
  ++impl->draws;
  impl->encodeNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count();
}
void Renderer::overlay(const std::shared_ptr<Image> &target,
                       const char *label) {
  if ((!label || !*label) && !vkbd::state().visible.load())
    return;
  if (!impl->font) {
    impl->font = std::make_shared<Image>();
    impl->font->width = kVkbdFontW;
    impl->font->height = kVkbdFontH;
    impl->font->rgba = {Bytes(kVkbdFontW * kVkbdFontH * 4, 255)};
    for (size_t i = 0; i < sizeof(kVkbdFont); ++i)
      impl->font->rgba[0][i * 4 + 3] = kVkbdFont[i];
    impl->font->rgba[0][3] = 255;
  }
  struct Vertex {
    float x, y, u, v, r, g, b, a;
  };
  std::vector<Vertex> vertices;
  float scale = std::max(1.0f, float(target->height) / 600);
  auto quad = [&](float x, float y, float w, float h, float u, float v,
                  float uw, float vh, std::array<float, 4> color) {
    float px[4] = {x, x + w, x + w, x}, py[4] = {y, y, y + h, y + h};
    unsigned indices[] = {0, 1, 2, 0, 2, 3};
    for (unsigned n : indices)
      vertices.push_back(
          {px[n] * 2 / target->width - 1, 1 - py[n] * 2 / target->height,
           u + (n == 1 || n == 2 ? uw : 0), v + (n >= 2 ? vh : 0), color[0],
           color[1], color[2], color[3]});
  };
  auto rect = [&](float x, float y, float w, float h,
                  std::array<float, 4> color) {
    quad(x, y, w, h, 0.5f / kVkbdFontW, 0.5f / kVkbdFontH, 0, 0, color);
  };
  auto text = [&](float x, float y, const std::string &value) {
    for (unsigned char ch : value) {
      if (ch >= 32 && ch < 128) {
        unsigned i = ch - 32;
        quad(x, y, 16 * scale, 16 * scale, float((i % 16) * 16) / kVkbdFontW,
             float((i / 16) * 16) / kVkbdFontH, 16.0f / kVkbdFontW,
             16.0f / kVkbdFontH, {1, 1, 1, 1});
      }
      x += 9 * scale;
    }
  };
  if (label && *label) {
    rect(10 * scale, 8 * scale, (std::strlen(label) * 9 + 12) * scale,
         22 * scale, {0, 0, 0, 0.7});
    text(16 * scale, 11 * scale, label);
  }
  if (vkbd::state().visible.load()) {
    auto &rows = vkbd::layout();
    float unit = 30 * scale, originX = (target->width - unit * 12) / 2,
          originY = target->height - (rows.size() + 1) * unit;
    rect(originX - 6 * scale, originY - 6 * scale, unit * 12 + 12 * scale,
         unit * rows.size() + 12 * scale, {0, 0, 0, 0.85});
    for (unsigned row = 0; row < rows.size(); ++row) {
      float x = originX;
      for (unsigned col = 0; col < rows[row].size(); ++col) {
        auto key = rows[row][col];
        float w = key.width * unit;
        bool selected = int(row) == vkbd::state().row.load() &&
                        int(col) == vkbd::state().col.load();
        rect(x, originY + row * unit, w - 3 * scale, unit - 3 * scale,
             selected ? std::array<float, 4>{0.1, 0.4, 0.7, 1}
                      : std::array<float, 4>{0.15, 0.15, 0.15, 1});
        char c = vkbd::state().shift.load() ? key.upper : key.lower;
        std::string name = key.special == vkbd::kEnter    ? "Enter"
                           : key.special == vkbd::kSpace  ? "Space"
                           : key.special == vkbd::kShift  ? "Shift"
                           : key.special == vkbd::kDelete ? "Del"
                                                          : std::string(1, c);
        text(x + 3 * scale, originY + row * unit + 5 * scale, name);
        x += w;
      }
    }
  }
  if (vertices.empty())
    return;
  Draw d;
  d.color = target;
  d.raster.depthOn = false;
  d.raster.depthWrite = false;
  d.raster.blendOn = true;
  d.raster.src = Blend::SrcAlpha;
  d.raster.dst = Blend::InvSrcAlpha;
  d.raster.srcAlpha = Blend::One;
  d.raster.dstAlpha = Blend::InvSrcAlpha;
  d.raster.viewport[2] = target->width;
  d.raster.viewport[3] = target->height;
  d.vs = std::make_shared<Shader>();
  d.vs->vertex = true;
  d.vs->entry = "overlayVS";
  d.vs->source = R"MSL(#include <metal_stdlib>
using namespace metal;
struct I{float2 p [[attribute(0)]];float2 uv [[attribute(1)]];float4 c [[attribute(2)]];};struct O{float4 p [[position]];float2 uv;float4 c;};vertex O overlayVS(I i [[stage_in]]){return {float4(i.p,0,1),i.uv,i.c};}
)MSL";
  d.ps = std::make_shared<Shader>();
  d.ps->entry = "overlayPS";
  d.ps->samplerMask = 1;
  d.ps->source = R"MSL(#include <metal_stdlib>
using namespace metal;
struct O{float4 p [[position]];float2 uv;float4 c;};fragment float4 overlayPS(O i [[stage_in]],texture2d<float> image [[texture(0)]],sampler smp [[sampler(0)]]){return i.c*image.sample(smp,i.uv);}
)MSL";
  for (auto s : {d.vs, d.ps})
    s->hash = shaderHash(s->source.data(), s->source.size());
  d.attributes = {{0, 0, 0, Element::Float2},
                  {1, 0, 8, Element::Float2},
                  {2, 0, 16, Element::Float4}};
  d.strides[0] = sizeof(Vertex);
  d.streams[0] = std::make_shared<Buffer>();
  d.streams[0]->bytes.resize(vertices.size() * sizeof(Vertex));
  std::memcpy(d.streams[0]->bytes.data(), vertices.data(),
              d.streams[0]->bytes.size());
  d.count = vertices.size();
  d.textures[0] = impl->font;
  d.samplers[0].address[0] = d.samplers[0].address[1] = 3;
  draw(d);
}

void Renderer::copy(const std::shared_ptr<Image> &from,
                    const std::shared_ptr<Image> &to, const uint32_t *sr,
                    const uint32_t *dr, bool linear) {
  @autoreleasepool {
    if (from == to)
      throw error("Overlapping Metal copy");
    impl->blit(impl->image(from), impl->image(to), sr, dr, linear);
  }
}
Bytes Renderer::read(const std::shared_ptr<Image> &image) {
  @autoreleasepool {
    auto texture = impl->image(image);
    wait();
    if (image->depth)
      throw error("Depth CPU readback unavailable");
    Bytes data(size_t(image->width) * image->height * 4);
    [texture getBytes:data.data()
          bytesPerRow:image->width * 4
           fromRegion:MTLRegionMake2D(0, 0, image->width, image->height)
          mipmapLevel:0];
    return data;
  }
}
void Renderer::setDebugCapture(bool enabled,bool color,bool materials){
  if(impl->debugEnabled!=enabled||impl->debugCaptureColor!=color||impl->debugCaptureMaterials!=materials){impl->resetDebugFrame();impl->debugEnabled=enabled;impl->debugCaptureColor=color;impl->debugCaptureMaterials=materials;}
}
std::shared_ptr<Image> Renderer::debugView(DebugView mode,float range){
  if(mode==DebugView::Off||!impl->debugEnabled)return {};
  impl->saveDebugDepth();
  if(!impl->debugValid)return {};
  if(!impl->debugOutput||impl->debugOutput->width!=impl->width||impl->debugOutput->height!=impl->height){
    impl->debugOutput=std::make_shared<Image>();impl->debugOutput->width=impl->width;
    impl->debugOutput->height=impl->height;impl->debugOutput->rgba={Bytes(size_t(impl->width)*impl->height*4)};
  }
  if(!impl->debugPS){
    impl->debugVS=std::make_shared<Shader>();impl->debugPS=std::make_shared<Shader>();
    for(auto shader:{impl->debugVS,impl->debugPS}){
      shader->source=debugViewSource;shader->hash=shaderHash(shader->source.data(),shader->source.size());
    }
    impl->debugVS->vertex=true;impl->debugVS->entry="debugVS";
    impl->debugPS->entry="debugPS";impl->debugPS->samplerMask=1;
  }
  Draw d;d.color=impl->debugOutput;d.vs=impl->debugVS;d.ps=impl->debugPS;d.count=3;
  d.raster.depthOn=d.raster.depthWrite=false;d.raster.viewport[2]=impl->width;d.raster.viewport[3]=impl->height;
  d.textures[0]=impl->debugDepth;
  float c[8];std::copy(impl->selectedProjection.values.begin(),impl->selectedProjection.values.end(),c);
  c[4]=std::isfinite(range)&&range>0?range:200;c[5]=mode==DebugView::Normals?1:0;
  c[6]=impl->selectedShift[0];c[7]=impl->selectedShift[1];
  d.psConstants.resize(sizeof c);std::memcpy(d.psConstants.data(),c,sizeof c);draw(d);
  if(impl->frames%300==0)std::printf("[FO2 DEBUG VIEW] %s %ux%u vertices=%llu projection=%.6g %.6g %.6g %.6g\n",
      mode==DebugView::Depth?"depth":"normals",impl->width,impl->height,(unsigned long long)impl->bestWeight,c[0],c[1],c[2],c[3]);
  return impl->debugOutput;
}
std::shared_ptr<Image> Renderer::screenReflections(const std::shared_ptr<Image>& base,bool mask,bool enabled){
  waterViewReady=false;
  if(impl->rtWaterOn)waterTraceState=1;
  if(!impl->debugEnabled||!impl->debugCaptureColor)return {};
  impl->saveDebugDepth();if(!impl->debugValid||!impl->debugScene)return {};
  if(impl->lateSceneReady&&(impl->lateSceneColorSource!=impl->selectedColorSource||impl->lateSceneDepthSource!=impl->selectedDepthSource))impl->lateSceneReady=false;
  Impl::WaterLayer* water=nullptr;
  for(auto& layer:impl->waterLayers)if(layer.active&&layer.sourceColor==impl->selectedColorSource&&layer.sourceDepth==impl->selectedDepthSource){water=&layer;break;}
  waterViewReady=water!=nullptr;
  if(!enabled)return base;
  auto makeImage=[](uint32_t w,uint32_t h){auto out=std::make_shared<Image>();out->width=w;out->height=h;out->rgba={Bytes(size_t(w)*h*4)};return out;};
  uint32_t w=(impl->width+1)/2,h=(impl->height+1)/2;
  if(!impl->ssrHalf||impl->ssrHalf->width!=w||impl->ssrHalf->height!=h)impl->ssrHalf=makeImage(w,h);
  if(!impl->debugOutput||impl->debugOutput->width!=impl->width||impl->debugOutput->height!=impl->height)impl->debugOutput=makeImage(impl->width,impl->height);
  if(!impl->ssrPS){
    auto source=std::string(debugViewSource)+ssrSource;
    auto shader=[&](const char* entry,bool vertex,uint32_t samplers){auto s=std::make_shared<Shader>();s->source=source;s->hash=shaderHash(source.data(),source.size());s->entry=entry;s->vertex=vertex;s->samplerMask=samplers;return s;};
    // Use a separate VS because the debug-view library may not yet exist.
    impl->ssrPS=shader("ssrPS",false,31);impl->ssrCompositePS=shader("ssrCompositePS",false,2047);
    if(!impl->debugVS)impl->debugVS=shader("debugVS",true,0);
  }
  float constants[16]{};std::copy(impl->selectedProjection.values.begin(),impl->selectedProjection.values.end(),constants);
  constants[5]=waterReflectionGain();
  constants[12]=water?1:0;constants[13]=worldWaterMaterial().reflectivity;constants[14]=worldWaterMaterial().roughness;constants[15]=float(unsigned(MaterialCue::WorldWater));
  constants[6]=impl->selectedShift[0];constants[7]=impl->selectedShift[1];constants[8]=w;constants[9]=h;constants[10]=mask?1:0;constants[11]=impl->debugCaptureMaterials?1:0;
  Draw d;d.color=impl->ssrHalf;d.vs=impl->debugVS;d.ps=impl->ssrPS;d.count=3;
  d.raster.depthOn=d.raster.depthWrite=false;d.raster.viewport[2]=w;d.raster.viewport[3]=h;
  d.textures[0]=impl->debugDepth;d.textures[1]=impl->lateSceneReady?impl->lateScene:impl->debugScene;d.textures[2]=impl->materialSaved;d.textures[3]=water?water->normals:nullptr;d.textures[4]=water?water->depth:impl->debugDepth;
  d.psConstants.resize(sizeof constants);std::memcpy(d.psConstants.data(),constants,sizeof constants);draw(d);
  std::shared_ptr<Image> rtWater;
  if(impl->rtWaterOn&&impl->frames%300==0)std::printf("[FO2 RT WATER GATE] frame=%llu requested=%d water=%d scene=%d parts=%zu shift=(%.9g,%.9g) raster-compatible=%d\n",
    (unsigned long long)impl->frames,int(waterTraceRequested.load()),int(water!=nullptr),int(impl->selectedWaterTrace.has_value()),
    impl->selectedWaterTrace?impl->selectedWaterTrace->parts.size():0,constants[6],constants[7],int(traceRasterShiftMatches(constants[6],constants[7],impl->width,impl->height)));
  if(impl->rtWaterOn&&waterTraceRequested.load()&&water&&!mask&&traceRasterShiftMatches(constants[6],constants[7],impl->width,impl->height))try{
    auto snapshot=std::move(impl->selectedWaterTrace);impl->selectedWaterTrace.reset();
    if(snapshot&&!snapshot->parts.empty()){
      // Finish this frame's game/SSR commands before the separate RT queue
      // reads their textures. No output survives for reuse by a later frame.
      impl->submit(true);
      if(!impl->waterTrace)impl->waterTrace=std::make_unique<WaterTrace>(impl->device);
      auto texture=impl->waterTrace->render(std::move(*snapshot),impl->frames,constants,impl->image(impl->debugDepth),
        impl->image(impl->lateSceneReady?impl->lateScene:impl->debugScene),impl->image(water->normals),impl->image(water->depth),
        [&](const std::shared_ptr<Image>& image,bool srgb){return impl->formatImage(image,srgb);},
        [&](const Sampler& sampler){return impl->sampler(sampler);});
      if(texture){rtWater=std::make_shared<Image>();rtWater->width=uint32_t(texture.width);rtWater->height=uint32_t(texture.height);
      auto gpu=std::make_shared<GPUImage>();gpu->value=texture;gpu->version=rtWater->version;rtWater->gpu=gpu;waterTraceState=2;}
    }
  }catch(const std::exception& e){impl->rtWaterOn=false;waterTraceState=3;impl->waterTraceScene.disable();impl->waterTrace.reset();
    std::fprintf(stderr,"[FO2 RT WATER] Disabled: %s; continuing with SSR\n",e.what());}
  impl->currentWaterTrace=rtWater;impl->currentWaterTraceFrame=impl->frames;
  constants[4]=rtWater?1:0;std::memcpy(d.psConstants.data(),constants,sizeof constants);d.textures[8]=rtWater;d.textures[9]=waterReplacementEnabled()&&water?water->legacy:nullptr;d.textures[10]=water?water->color:nullptr;
  d.color=impl->debugOutput;d.ps=impl->ssrCompositePS;d.raster.viewport[2]=impl->width;d.raster.viewport[3]=impl->height;
  d.textures[0]=base;d.textures[1]=impl->lateSceneReady?impl->lateScene:impl->debugScene;d.textures[2]=impl->ssrHalf;d.textures[3]=impl->materialSaved;d.textures[4]=water?water->normals:nullptr;d.textures[5]=impl->lateSceneReady?impl->lateScene:(water?water->color:impl->debugScene);d.textures[6]=impl->debugDepth;d.textures[7]=water?water->depth:impl->debugDepth;draw(d);
  if(impl->frames%300==0){
    std::printf("[FO2 SSR] %s %ux%u 48 steps, %s receiver mask; confidence-weighted surface resolve\n",mask?"mask":"on",w,h,impl->debugCaptureMaterials?"material":"geometric");
    if(impl->debugCaptureMaterials){std::printf("[FO2 MATERIALS] draws: unknown=%llu environment-cube=%llu fixed-specular=%llu water=%llu terrain-specular=%llu\n",(unsigned long long)impl->materialDraws[0],(unsigned long long)impl->materialDraws[1],(unsigned long long)impl->materialDraws[2],(unsigned long long)impl->materialDraws[3],(unsigned long long)impl->materialDraws[4]);impl->materialDraws={};}
    std::printf("[FO2 WATER] matched=%d captured=%llu dropped=%llu\n",water?1:0,(unsigned long long)impl->waterDraws,(unsigned long long)impl->waterDropped);impl->waterDraws=impl->waterDropped=0;
  }
  const char* captureRT=std::getenv("FO2_RT_WATER_CAPTURE");
  if(rtWater&&impl->waterTrace&&impl->waterTrace->waterRayCount()>=4096&&impl->rtAutoInspections<3&&
     (impl->rtAutoInspections==0||impl->frames>=impl->rtInspectionFrame+300)&&(!captureRT||std::strcmp(captureRT,"0"))){
    const char* session=std::getenv("FO2_METAL_SESSION_DIR");if(session&&*session){
      ++impl->rtAutoInspections;impl->rtInspectionFrame=impl->frames;
      captureReflectionDiagnostics((std::filesystem::path(session)/"rt-water-inspect").string(),base);
      if(impl->waterTrace->rayOnlyMaterialHitCount()>=16)impl->rayOnlyInspected=true;
      if(impl->waterTrace->detachedMaterialHitCount()>=16)impl->detachedInspected=true;
    }
  }
  if(rtWater&&impl->waterTrace&&!impl->rayOnlyInspected&&impl->waterTrace->rayOnlyMaterialHitCount()>=16&&
     (!captureRT||std::strcmp(captureRT,"0"))){
    const char* session=std::getenv("FO2_METAL_SESSION_DIR");if(session&&*session){
      impl->rayOnlyInspected=true;
      captureReflectionDiagnostics((std::filesystem::path(session)/"rt-ray-only-inspect").string(),base);
      if(impl->waterTrace->detachedMaterialHitCount()>=16)impl->detachedInspected=true;
    }
  }
  if(rtWater&&impl->waterTrace&&!impl->detachedInspected&&impl->waterTrace->detachedMaterialHitCount()>=16&&
     (!captureRT||std::strcmp(captureRT,"0"))){
    const char* session=std::getenv("FO2_METAL_SESSION_DIR");if(session&&*session){
      impl->detachedInspected=true;
      captureReflectionDiagnostics((std::filesystem::path(session)/"rt-detached-inspect").string(),base);
    }
  }
  return impl->debugOutput;
}
bool Renderer::captureReflectionDiagnostics(const std::string& directory,const std::shared_ptr<Image>& base){
  if(!impl->debugEnabled||!impl->debugValid||!impl->debugScene||!impl->ssrPS)return false;
  if(base&&(base->width!=impl->width||base->height!=impl->height))return false;
  try{
    auto folder=std::filesystem::path(directory)/("slot-"+std::to_string(impl->reflectionInspections++%8));
    std::filesystem::create_directories(folder);
    std::filesystem::remove(folder/"inspection.json");
    Impl::WaterLayer* water=nullptr;
    for(auto& layer:impl->waterLayers)if(layer.active&&layer.sourceColor==impl->selectedColorSource&&layer.sourceDepth==impl->selectedDepthSource){water=&layer;break;}
    auto makeImage=[](uint32_t w,uint32_t h){auto out=std::make_shared<Image>();out->width=w;out->height=h;out->rgba={Bytes(size_t(w)*h*4)};return out;};
    // Render into private inspection targets: neither displayed output nor the
    // cached reflection image may be overwritten by screenshot diagnostics.
    auto mask=makeImage(impl->width,impl->height),hits=makeImage((impl->width+1)/2,(impl->height+1)/2);
    float c[16]{};std::copy(impl->selectedProjection.values.begin(),impl->selectedProjection.values.end(),c);
    c[5]=waterReflectionGain();
    c[6]=impl->selectedShift[0];c[7]=impl->selectedShift[1];c[11]=impl->debugCaptureMaterials?1:0;
    c[12]=water?1:0;c[13]=worldWaterMaterial().reflectivity;c[14]=worldWaterMaterial().roughness;c[15]=float(unsigned(MaterialCue::WorldWater));
    Draw d;d.vs=impl->debugVS;d.ps=impl->ssrPS;d.count=3;d.raster.depthOn=d.raster.depthWrite=false;
    d.textures[0]=impl->debugDepth;d.textures[1]=impl->lateSceneReady?impl->lateScene:impl->debugScene;d.textures[2]=impl->materialSaved;
    d.textures[3]=water?water->normals:nullptr;d.textures[4]=water?water->depth:impl->debugDepth;
    auto render=[&](const std::shared_ptr<Image>& target,bool receiverMask){
      d.color=target;d.raster.viewport[2]=target->width;d.raster.viewport[3]=target->height;
      c[8]=target->width;c[9]=target->height;c[10]=receiverMask?1:0;
      d.psConstants.resize(sizeof c);std::memcpy(d.psConstants.data(),c,sizeof c);draw(d);return read(target);
    };
    auto receivers=render(mask,true),reflections=render(hits,false);
    auto save=[&](const char* name,const Bytes& bytes,unsigned w,unsigned h){
      std::ofstream f(folder/name,std::ios::binary);f.exceptions(std::ios::badbit|std::ios::failbit);f<<"P6\n"<<w<<' '<<h<<"\n255\n";
      for(size_t i=0;i<bytes.size();i+=4)f.write(reinterpret_cast<const char*>(bytes.data()+i),3);
    };
    save("receivers.ppm",receivers,mask->width,mask->height);
    save("reflection-radiance.ppm",reflections,hits->width,hits->height);
    save("opaque-scene.ppm",read(impl->debugScene),impl->width,impl->height);
    if(!impl->inspectionDepthPS){
      impl->inspectionDepthPS=std::make_shared<Shader>();auto& shader=*impl->inspectionDepthPS;
      shader.entry="inspectionDepthPS";shader.samplerMask=1;
      shader.source=R"MSL(#include <metal_stdlib>
using namespace metal;
fragment float4 inspectionDepthPS(float4 p [[position]],depth2d<float,access::read> image [[texture(0)]]){
 uint bits=as_type<uint>(image.read(uint2(p.xy)));
 return float4(bits&255u,(bits>>8)&255u,(bits>>16)&255u,(bits>>24)&255u)/255.0;
}
)MSL";
      shader.hash=shaderHash(shader.source.data(),shader.source.size());
    }
    auto saveDepth=[&](const std::shared_ptr<Image>& depth,const char* name){
      auto target=makeImage(impl->width,impl->height);Draw inspect;
      inspect.color=target;inspect.vs=impl->debugVS;inspect.ps=impl->inspectionDepthPS;inspect.count=3;
      inspect.raster.depthOn=inspect.raster.depthWrite=false;inspect.raster.viewport[2]=impl->width;inspect.raster.viewport[3]=impl->height;
      inspect.textures[0]=depth;draw(inspect);auto bytes=read(target);
      std::ofstream file(folder/name,std::ios::binary);file.exceptions(std::ios::badbit|std::ios::failbit);
      file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    };
    saveDepth(impl->debugDepth,"opaque-depth.f32");
    if(water){saveDepth(water->depth,"water-depth.f32");save("water-normals.ppm",read(water->normals),impl->width,impl->height);}

    if(impl->lateSceneReady)save("pre-hud-scene.ppm",read(impl->lateScene),impl->width,impl->height);
    if(water)save("water-pass-scene.ppm",read(water->color),impl->width,impl->height);
    uint64_t replacementPixels=0;
    if(waterReplacementEnabled()&&water&&water->legacy){auto metadata=read(water->legacy);
      save("water-legacy-delta.ppm",metadata,impl->width,impl->height);
      for(size_t i=3;i<metadata.size();i+=4)if(metadata[i]>1)++replacementPixels;
    }
    Bytes strength=reflections;uint64_t receiverPixels=0,waterPixels=0,hitPixels=0,waterHitPixels=0;
    auto isWater=[&](size_t i){return receivers[i]==0&&receivers[i+1]==153&&receivers[i+2]==204;};
    for(size_t i=0;i<receivers.size();i+=4){if(isWater(i))++waterPixels;if(receivers[i]||receivers[i+1]||receivers[i+2])++receiverPixels;}
    for(unsigned y=0;y<hits->height;++y)for(unsigned x=0;x<hits->width;++x){
      size_t i=(size_t(y)*hits->width+x)*4;const auto alpha=reflections[i+3];
      strength[i]=strength[i+1]=strength[i+2]=alpha;
      if(alpha){++hitPixels;unsigned px=std::min(unsigned((x+.5)*mask->width/hits->width),mask->width-1);
        unsigned py=std::min(unsigned((y+.5)*mask->height/hits->height),mask->height-1);
        if(isWater((size_t(py)*mask->width+px)*4))++waterHitPixels;}
    }
    save("reflection-strength.ppm",strength,hits->width,hits->height);
    std::array<uint64_t,5> cues{};
    if(impl->debugCaptureMaterials&&impl->materialSaved){auto material=read(impl->materialSaved);save("material-raw.ppm",material,impl->width,impl->height);
      for(size_t i=0;i<material.size();i+=4)if(material[i+2]<cues.size())++cues[material[i+2]];
    }
    uint64_t changedPixels=0,significantPixels=0,waterChangedPixels=0;
    uint64_t rtChanged=0,rtSignificant=0,rtWaterSignificant=0,rtNonwaterChanged=0;unsigned rtMaxDelta=0;
    bool rtCompared=base&&impl->currentWaterTrace&&impl->currentWaterTraceFrame==impl->frames;

    if(base){
      auto composite=makeImage(impl->width,impl->height);d.color=composite;d.ps=impl->ssrCompositePS;
      d.raster.viewport[2]=impl->width;d.raster.viewport[3]=impl->height;
      d.textures[0]=base;d.textures[1]=impl->lateSceneReady?impl->lateScene:impl->debugScene;d.textures[2]=hits;d.textures[3]=impl->materialSaved;
      d.textures[4]=water?water->normals:nullptr;d.textures[5]=impl->lateSceneReady?impl->lateScene:(water?water->color:impl->debugScene);
      d.textures[6]=impl->debugDepth;d.textures[7]=water?water->depth:impl->debugDepth;
      draw(d);auto before=read(base),after=read(composite),difference=before;
      for(size_t i=0;i<before.size();i+=4){unsigned maximum=0;
        for(unsigned k=0;k<3;++k){unsigned delta=unsigned(std::abs(int(before[i+k])-int(after[i+k])));
          maximum=std::max(maximum,delta);difference[i+k]=uint8_t(std::min(255u,delta*8));}
        if(maximum)++changedPixels;if(maximum>=3){++significantPixels;if(isWater(i))++waterChangedPixels;}
      }
      save("base.ppm",before,impl->width,impl->height);save("composite.ppm",after,impl->width,impl->height);
      save("difference-x8.ppm",difference,impl->width,impl->height);
      if(rtCompared){
        c[4]=1;d.psConstants.resize(sizeof c);std::memcpy(d.psConstants.data(),c,sizeof c);
        d.textures[8]=impl->currentWaterTrace;d.textures[9]=waterReplacementEnabled()&&water?water->legacy:nullptr;d.textures[10]=water?water->color:nullptr;draw(d);auto hybrid=read(composite),rtDifference=after;
        for(size_t i=0;i<after.size();i+=4){unsigned maximum=0;
          for(unsigned k=0;k<3;++k){unsigned delta=unsigned(std::abs(int(after[i+k])-int(hybrid[i+k])));
            maximum=std::max(maximum,delta);rtDifference[i+k]=uint8_t(std::min(255u,delta*8));}
          rtMaxDelta=std::max(rtMaxDelta,maximum);if(maximum){++rtChanged;if(!isWater(i))++rtNonwaterChanged;}
          if(maximum>=3){++rtSignificant;if(isWater(i))++rtWaterSignificant;}
        }
        save("rt-composite.ppm",hybrid,impl->width,impl->height);
        save("rt-minus-ssr-x8.ppm",rtDifference,impl->width,impl->height);
        if(!impl->inspectionReflectionPS){impl->inspectionReflectionPS=std::make_shared<Shader>();
          auto& shader=*impl->inspectionReflectionPS;shader.source=std::string(debugViewSource)+ssrSource;
          shader.hash=shaderHash(shader.source.data(),shader.source.size());shader.entry="reflectionCopyPS";shader.samplerMask=1;}
        auto raw=makeImage(impl->currentWaterTrace->width,impl->currentWaterTrace->height);
        Draw inspect;inspect.color=raw;inspect.vs=impl->debugVS;inspect.ps=impl->inspectionReflectionPS;inspect.count=3;
        inspect.raster.depthOn=inspect.raster.depthWrite=false;inspect.raster.viewport[2]=raw->width;inspect.raster.viewport[3]=raw->height;
        inspect.textures[0]=impl->currentWaterTrace;draw(inspect);auto bytes=read(raw),strength=bytes;
        for(size_t i=0;i<strength.size();i+=4)strength[i]=strength[i+1]=strength[i+2]=uint8_t(std::min(255u,unsigned(bytes[i+3])*8));
        save("rt-radiance.ppm",bytes,raw->width,raw->height);save("rt-strength-x8.ppm",strength,raw->width,raw->height);
        std::printf("[FO2 RT WATER INSPECT] frame=%llu same-frame RT vs SSR: changed=%llu water>=3=%llu nonwater=%llu max-delta=%u; %s\n",
          (unsigned long long)impl->frames,(unsigned long long)rtChanged,(unsigned long long)rtWaterSignificant,
          (unsigned long long)rtNonwaterChanged,rtMaxDelta,folder.c_str());
      }

    }
    NSDictionary* report=@{@"frame":@(impl->frames),@"ssr_display_enabled":@(fo2MetalSSREnabled()),@"water_layer_matched":@(water!=nullptr),
      @"depth_encoding":@"little-endian-float32",@"ssr_resolve":@"confidence-weighted-surface-bilinear-v1",@"projection":@[@(c[0]),@(c[1]),@(c[2]),@(c[3])],
      @"late_scene_reference":@(impl->lateSceneReady),@"receiver_pixels":@(receiverPixels),@"visible_water_receiver_pixels":@(waterPixels),@"half_resolution_nonzero_confidence_pixels":@(hitPixels),
      @"half_resolution_water_nonzero_confidence_pixels":@(waterHitPixels),@"opaque_unknown_pixels":@(cues[0]),@"opaque_cube_pixels":@(cues[1]),
      @"opaque_fixed_specular_pixels":@(cues[2]),@"opaque_water_cue_pixels":@(cues[3]),@"opaque_terrain_specular_pixels":@(cues[4]),
      @"rt_same_frame_compared":@(rtCompared),@"rt_source_frame":@(impl->currentWaterTraceFrame),
      @"water_reflection_gain":@(waterReflectionGain()),
      @"water_legacy_replacement_enabled":@(waterReplacementEnabled()),@"water_legacy_replacement_pixels":@(replacementPixels),
      @"rt_near_visible_material_hits":@(rtCompared&&impl->waterTrace?impl->waterTrace->nearVisibleMaterialHitCount():0),
      @"rt_ray_only_material_hits":@(rtCompared&&impl->waterTrace?impl->waterTrace->rayOnlyMaterialHitCount():0),
      @"rt_detached_material_hits":@(rtCompared&&impl->waterTrace?impl->waterTrace->detachedMaterialHitCount():0),
      @"rt_vs_ssr_changed_pixels":@(rtChanged),@"rt_vs_ssr_changed_pixels_at_least_3_levels":@(rtSignificant),
      @"rt_vs_ssr_water_changed_pixels_at_least_3_levels":@(rtWaterSignificant),@"rt_vs_ssr_nonwater_changed_pixels":@(rtNonwaterChanged),@"rt_vs_ssr_max_channel_delta":@(rtMaxDelta),
      @"fresh_inspection_even_when_ssr_off":@YES,@"composite_compared":@(base!=nullptr),
      @"changed_pixels":@(changedPixels),@"changed_pixels_at_least_3_levels":@(significantPixels),@"water_changed_pixels_at_least_3_levels":@(waterChangedPixels),@"width":@(impl->width),@"height":@(impl->height)};
    NSError* error=nil;NSData* json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:&error];
    if(!json||![json writeToFile:[NSString stringWithUTF8String:(folder/"inspection.json").c_str()] options:NSDataWritingAtomic error:&error])
      throw std::runtime_error("SSR inspection report write failed");
    std::printf("[FO2 SSR INSPECT] frame=%llu receivers=%llu water=%llu hits=%llu water-hits=%llu changed=%llu water-changed=%llu: %s\n",
      (unsigned long long)impl->frames,(unsigned long long)receiverPixels,(unsigned long long)waterPixels,
      (unsigned long long)hitPixels,(unsigned long long)waterHitPixels,(unsigned long long)significantPixels,(unsigned long long)waterChangedPixels,folder.c_str());
    return true;
  }catch(const std::exception& error){std::fprintf(stderr,"[FO2 SSR INSPECT] Diagnostic failed: %s\n",error.what());return false;}
}
void Renderer::present(const std::shared_ptr<Image> &image) {
  if(!impl->frames)std::printf("[FO2 STARTUP] first Metal present %.3f seconds after renderer initialization\n",std::chrono::duration<double>(std::chrono::steady_clock::now()-impl->lastReport).count());
  @autoreleasepool {
    if (impl->layer) {
      id<CAMetalDrawable> drawable = [impl->layer nextDrawable];
      if (drawable) {
        impl->blit(impl->image(image), drawable.texture);
        [impl->command presentDrawable:drawable];
      }
    }
    impl->submit(false);
    ++impl->frames;
    impl->physicalWater.report(impl->frames);
    impl->traceCapture.present(fo2MetalRaceActive(),impl->frames);
    if(impl->liveTrace.enabled())try{
      bool racing=fo2MetalRaceActive();
      if(!racing&&impl->traceWasRacing&&impl->traceUpdates)impl->traceUpdates->invalidate();impl->traceWasRacing=racing;
      if(impl->traceUpdates&&impl->traceUpdates->failed())impl->liveTrace.disable();
      else {auto snapshot=impl->liveTrace.present(racing,impl->frames,!impl->traceUpdates||impl->traceUpdates->canAccept());
        if(snapshot){if(!impl->traceUpdates){impl->traceUpdates=std::make_unique<TraceUpdates>();std::puts("[FO2 RT UPDATE] Worker started after racing warmup; startup/menu had no RT thread.");}
          impl->traceUpdates->submit(std::move(*snapshot));}}
    }catch(const std::exception& error){impl->liveTrace.disable();if(impl->traceUpdates)impl->traceUpdates->invalidate();std::fprintf(stderr,"[FO2 RT UPDATE] Disabled: %s; gameplay remains SSR\n",error.what());}
    impl->resetDebugFrame();
    impl->waterTraceScene.beginDisplayFrame(fo2MetalRaceActive()&&impl->rtWaterOn&&waterTraceRequested.load(),impl->frames);
    if (impl->frames % 300 == 0) {
      auto now = std::chrono::steady_clock::now();
      double elapsed =
          std::chrono::duration<double>(now - impl->lastReport).count();
      auto completed = impl->metrics->completed.exchange(0);
      auto ns = impl->metrics->gpuNs.exchange(0);
      std::printf(
          "[FO2 METAL] frame=%llu %.1f FPS, %.1f draws/frame, CPU encode %.3f "
          "ms/frame, GPU %.3f ms/submission, pipelines=%zu, "
          "passes/frame=%.1f\n",
          (unsigned long long)impl->frames, 300.0 / elapsed,
          impl->draws / 300.0, impl->encodeNs / 300e6,
          completed ? ns / (completed * 1e6) : 0, impl->pipelines.size()+impl->blitPipelines.size(),
          impl->passes / 300.0);
      impl->lastReport = now;
      impl->draws = impl->encodeNs = impl->passes = 0;
    }
  }
}
void Renderer::wait() {
  impl->submit(true);
  dispatch_semaphore_wait(impl->slots, DISPATCH_TIME_FOREVER);
  dispatch_semaphore_wait(impl->slots, DISPATCH_TIME_FOREVER);
  dispatch_semaphore_signal(impl->slots);
  dispatch_semaphore_signal(impl->slots);
}
} // namespace fo2render
