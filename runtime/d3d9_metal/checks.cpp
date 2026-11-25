#include "../render/commands.h"
#include "../render/particle_strip.h"
#include "../render/shader_capture.h"
#include "../render/validate.h"
#include "../render/material_shader.h"
#include "../render/water.h"
#include "../render/physical_water.h"
#include "../render/water_replacement.h"
#include "../render/terrain_specular.h"
#include "debug_views.h"
#include "ssr.h"
#include "trace_car_fixture.h"
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
extern int fo2CheckMetalAdapter();
extern int fo2CheckMetalFixedShaders(fo2render::Renderer *);
namespace fo2render {
static void require(bool ok, const char *text) {
  if (!ok)
    throw std::runtime_error(text);
}
static Bytes bytecode(std::initializer_list<uint32_t> words) {
  Bytes b(words.size() * 4);
  std::memcpy(b.data(), words.begin(), b.size());
  return b;
}
static Bytes terrainBytecode(){
  return bytecode({0xffff0101,0x42,0xb00f0000,0x42,0xb00f0001,5,0x800f0000,
      0xb0e40000,0xb0e40001,4,0x80170000,0x90e40000,0x80ff0000,0x80e40000,0xffff});
}
static auto testVS() {
  return translateShader(
      bytecode({0xfffe0200, 0x0200001f, 0x80000000, 0x900f0000, 0x02000001,
                0xc00f0000, 0x90e40000, 0x0000ffff}),
      true);
}
static auto testPS() {
  return translateShader(
      bytecode({0xffff0200, 0x02000001, 0x800f0800, 0xa0e40000, 0x0000ffff}),
      false);
}
// A literal-only pixel shader has no original MSL arguments. This was the first
// desktop compiler failure: appending effects must not produce a leading comma.
static auto testLiteralPS(bool fog = false) {
  return translateShader(
      bytecode({0xffff0200, 0x05000051, 0xa00f0000, 0x3f800000, 0x3e800000,
                0x3f000000, 0x3f800000, 0x02000001, 0x800f0800, 0xa0e40000,
                0x0000ffff}),
      false, 0, 0, fog);
}
static auto testLegacyPS(unsigned dependent = 0, bool fog = false) {
  auto code =
      dependent ? bytecode({0xffff0101, 66, 0xb00f0000, dependent, 0xb00f0001,
                            0xb0e40000, 1, 0x800f0000, 0xb0e40001, 0x0000ffff})
                : bytecode({0xffff0101, 66, 0xb00f0000, 5, 0x800f0000,
                            0xb0e40000, 0x90e40000, 0x0000ffff});
  return translateShader(code, false, 0, dependent == 82 ? 2 : 0, fog);
}
static void checkLegacyShaders(Renderer *renderer) {
  for (unsigned op : {0u, 69u, 70u, 82u})
    for (bool fog : {false, true}) {
      auto shader = testLegacyPS(op, fog);
      require(shader->samplerMask == (op ? 3u : 1u),
              "Legacy sampler reflection mismatch");
      require(shader->cubeSamplerMask == (op == 82 ? 2u : 0u),
              "Legacy cube sampler reflection mismatch");
      require(shader->source.find("t0 [[user(texcoord0)]]") !=
                  std::string::npos,
              "Legacy texture input missing");
      if (op) {
        require(shader->source.find("input.t1") == std::string::npos,
                "Dependent read uses an unwritten interpolator");
        require(shader->source.find(op == 82 ? "s1_texture.sample(s1, t1.xyz"
                                             : "s1_texture.sample(s1, t1.xy") !=
                    std::string::npos,
                "Dependent texture sampler/coordinates mismatch");
      }
      if (renderer)
        renderer->validateShader(shader);
    }
}
static auto testMatrixVS(unsigned op, bool alias) {
  unsigned rows = op == 20 || op == 22 ? 4 : op == 24 ? 2 : 3;
  return translateShader(
      bytecode({0xfffe0101, 1, 0x800f0000, 0x90e40000, 1, 0xb0010000,
                0xa0000000, op, 0x80000000u | (((1u << rows) - 1) << 16),
                alias ? 0x80e40000u : 0x90e40000u, 0xa0e42025, 1, 0xc00f0000,
                0x80e40000, 0x0000ffff}),
      true);
}
static void checkMatrixShaders(Renderer *renderer) {
  for (unsigned op : {20u, 21u, 22u, 23u, 24u})
    for (bool alias : {false, true}) {
      auto shader = testMatrixVS(op, alias);
      require(shader->source.find("r0.x = dot(") != std::string::npos,
              "Matrix was not lowered to scalar dots");
      require(shader->source.find("#define c0 uniforms.uniforms_float4[0];") ==
                  std::string::npos,
              "Uniform alias has a statement terminator");
      if (renderer)
        renderer->validateShader(shader);
    }
}
static void cpuChecks() {
  {
    const auto bytes=terrainBytecode();
    require(terrainSpecularProgram(shaderHash(bytes.data(),bytes.size()),bytes.size()),"Authored terrain program identity mismatch");
    require(!terrainSpecularProgram(0x40e19e2958b0afb0ull,60)&&!terrainSpecularProgram(0x40e19e2958b0afb1ull,64),"Unrelated terrain program accepted");
    require(terrainSpecularPass(3,0,true,true,false,false,false),"Opaque terrain specular pass rejected");
    require(!terrainSpecularPass(3,1,true,true,false,false,false)&&!terrainSpecularPass(1,0,true,true,false,false,false)&&
      !terrainSpecularPass(3,0,false,true,false,false,false)&&!terrainSpecularPass(3,0,true,false,false,false,false)&&
      !terrainSpecularPass(3,0,true,true,true,false,false)&&!terrainSpecularPass(3,0,true,true,false,true,false)&&
      !terrainSpecularPass(3,0,true,true,false,false,true),"Unsupported terrain pass accepted");
    auto shader=translateShader(bytes,false);
    require(shader->terrainSpecularMask,"Authored terrain mask not attached to translated shader");
    auto variant=materialOutputSource(shader->source,true);
    require(variant.find("clamp(t0.a*t1.a,0.0,1.0)")!=std::string::npos&&
      variant.find("discard_fragment")!=std::string::npos&&shader->source.find("fo2material")==std::string::npos,
      "Terrain mask lost authored alpha, alpha test or original shader preservation");
    require(!translateShader(bytes,false,0,1)->terrainSpecularMask,"Cube variant accepted as 2D terrain mask");
    bool rejected=false;try{materialOutputSource(testPS()->source,true);}catch(...){rejected=true;}
    require(rejected,"Unknown terrain shader layout accepted");
    std::puts("[Metal check] CPU terrain: authored specular program, render-state guards and two-texture alpha mask passed");
  }
  {
    require(worldWaterProgram(0xf65220e267055268ull,164,12,true,true,false,false,false),"Verified WorldWater program rejected");
    require(!worldWaterProgram(0xf65220e267055269ull,164,12,true,true,false,false,false),"Unrelated shader classified as water");
    require(!worldWaterProgram(0xf65220e267055268ull,164,12,true,true,true,false,false),"Depth-writing shader classified as water");
    std::array<float,9> basis{};float camera[16]={2,0,0,0,0,3,0,0,0,0,1.01f,-.1f,0,0,1,0};
    require(waterViewBasis(camera,basis)&&basis[0]==1&&basis[4]==1&&basis[8]==-1,"Water world/view normal basis incorrect");
    camera[0]=std::numeric_limits<float>::quiet_NaN();require(!waterViewBasis(camera,basis),"Invalid water basis accepted");
    bool rejected=false;try{waterNormalSource("return output;");}catch(...){rejected=true;}require(rejected,"Unknown water shader layout accepted");
    std::puts("[Metal check] CPU water: verified program/state recognition, normal basis and unsupported shader rejection passed");
  }
  {
    require(classifyMaterial(0,true,true,1,1,1,32).cue==MaterialCue::Unknown,"Programmable shader incorrectly uses fixed material state");
    auto cube=classifyMaterial(1,true,false,0,0,0,0);require(cube.cue==MaterialCue::EnvironmentCube&&cube.reflectivity>0,"Sampled environment cube not classified");
    auto fixed=classifyMaterial(0,false,true,.5f,.3f,.2f,30);require(fixed.cue==MaterialCue::FixedSpecular&&fixed.reflectivity==.5f&&fixed.roughness==.25f,"Fixed specular material mapping failed");
    require(classifyMaterial(0,false,true,1,1,1,std::numeric_limits<float>::quiet_NaN()).cue==MaterialCue::Unknown,"Nonfinite material accepted");
    auto original=testPS();auto augmented=materialOutputSource(original->source);
    require(augmented.find("[[color(1)]]")!=std::string::npos&&augmented.find("discard_fragment")!=std::string::npos,"Material output lost alpha-test handling");
    require(original->source.find("fo2material")==std::string::npos,"Material variant mutated original shader");
    std::string fixedSource="fragment float4 fo2_fragment(constant float* fo2fx [[buffer(30)]]){float4 cur(1);return cur;}";
    require(materialOutputSource(fixedSource).find("Fo2MaterialOutput{cur,")!=std::string::npos,"Fixed material output unsupported");
    bool rejected=false;try{materialOutputSource("fragment float4 other(){return float4(0);}");}catch(...){rejected=true;}
    require(rejected,"Unrecognized material output silently accepted");
    std::puts("[Metal check] CPU materials: reflective cues, unknown/invalid state and programmable/fixed output preservation passed");
  }
  {
    Shader ray,composite;ray.hash=composite.hash=123;ray.entry="ssrPS";composite.entry="ssrCompositePS";
    require(pipelineShaderIdentity(ray)!=pipelineShaderIdentity(composite),"Pipeline aliases different functions from one Metal library");
    composite=ray;require(pipelineShaderIdentity(ray)==pipelineShaderIdentity(composite),"Identical shader identity cannot reuse pipeline");
    composite.vertex=true;require(pipelineShaderIdentity(ray)!=pipelineShaderIdentity(composite),"Pipeline shader identity ignores stage");
    std::puts("[Metal check] CPU pipeline identity: library hash, entry point and stage distinguished");
  }
  {
    struct Vertex {
      float x, y, z;
      uint32_t color;
      float u, v;
    };
    static_assert(sizeof(Vertex) == 24);
    Vertex v[] = {{1, 2, 3, 0xffffffff, 0, 0},
                  {1.1f, 2, 3, 0xffffffff, 0, 1},
                  {},
                  {1000, 1000, 1000, 0xffffffff, 1, 1}};
    require(repairParticleStrip(reinterpret_cast<uint8_t *>(v), 4) == 2,
            "Particle gaps/tails were not collapsed");
    require(!std::memcmp(v + 1, v + 2, 24) && !std::memcmp(v + 2, v + 3, 24),
            "Particle repair left a long triangle");
    Vertex connectors[] = {{1, 2, 3, 1, 0, 0},
                           {1, 2, 3, 1, 0, 0},
                           {100, 2, 3, 1, 0, 0},
                           {100, 2, 3, 1, 0, 0}};
    require(repairParticleStrip(reinterpret_cast<uint8_t *>(connectors), 4) ==
                0,
            "Legitimate strip connector changed");
    Vertex brokenConnector[] = {{1, 2, 3, 1, 0, 0},
                                {1.1f, 2, 3, 1, 0, 1},
                                {100, 2, 3, 1, 1, 0},
                                {100, 2, 3, 1, 1, 0}};
    require(repairParticleStrip(reinterpret_cast<uint8_t *>(brokenConnector), 4) == 2,
            "One doubled endpoint left a long particle triangle");
    require(!std::memcmp(brokenConnector + 1, brokenConnector + 2, 24) &&
                !std::memcmp(brokenConnector + 1, brokenConnector + 3, 24),
            "Incomplete connector was not made degenerate");
    Vertex partial[] = {{0, 0, 0, 0xffffffff, 0, 1},
                         {1, 2, 3, 0xffffffff, 0, 0},
                         {1.1f, 2, 3, 0xffffffff, 0, 1},
                         {0, 0, 0, 0xffffffff, 1, 1},
                         {0, 0, 0, 0xffffffff, 1, 1}};
    require(repairParticleStrip(reinterpret_cast<uint8_t *>(partial), 5) == 3,
            "Partially written positions were mistaken for strip connectors");
    require(!std::memcmp(partial, partial + 1, 24) &&
                !std::memcmp(partial + 2, partial + 3, 24) &&
                !std::memcmp(partial + 2, partial + 4, 24),
            "Partial particle slots still connect to the origin");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    Vertex invalid[] = {{1, 2, 3, 0xffffffff, 0, 0},
                         {nan, 0, 0, 0xffffffff, 1, 1},
                         {nan, 0, 0, 0xffffffff, 1, 1},
                         {std::numeric_limits<float>::infinity(), 0, 0, 1, 0, 0}};
    require(repairParticleStrip(reinterpret_cast<uint8_t *>(invalid), 4) == 3,
            "Repeated nonfinite positions were preserved as connectors");
    for (unsigned i = 1; i < 4; ++i)
      require(!std::memcmp(invalid, invalid + i, 24),
              "Nonfinite particle position reached the GPU");
    Vertex entirelyInvalid[] = {{nan, 0, 0, 1, 0, 0}, {0, 0, 0, 1, 0, 1}};
    require(repairParticleStrip(reinterpret_cast<uint8_t *>(entirelyInvalid), 2) == 2,
            "Entirely invalid strip was not neutralized");
    for (auto &v : entirelyInvalid)
      require(v.x == 0 && v.y == 0 && v.z == 0 && !v.color,
              "Entirely invalid strip is not degenerate");
    Vertex zero[4]{};
    require(!repairParticleStrip(reinterpret_cast<uint8_t *>(zero), 4),
            "Empty particle strip changed");
  }

  {
    Draw d;
    d.count = 3;
    d.attributes = {{0, 0, 0, Element::Float4}};
    d.strides[0] = 16;
    d.streams[0] = std::make_shared<Buffer>();
    d.streams[0]->bytes.resize(48);
    validateDrawBuffers(d);
    d.indices = std::make_shared<Buffer>();
    uint16_t indices[] = {1, 2, 3};
    d.indices->bytes.resize(sizeof(indices));
    std::memcpy(d.indices->bytes.data(), indices, sizeof(indices));
    d.baseVertex = -1;
    validateDrawBuffers(d);
    d.baseVertex = 0;
    bool rejected = false;
    try {
      validateDrawBuffers(d);
    } catch (const std::exception &) {
      rejected = true;
    }
    require(rejected, "Out-of-bounds vertex fetch accepted");
    d.baseVertex = -2;
    rejected = false;
    try {
      validateDrawBuffers(d);
    } catch (const std::exception &) {
      rejected = true;
    }
    require(rejected, "Negative base vertex accepted");
    d.strides[0] = 0;
    d.baseVertex = 0;
    d.streams[0]->bytes.resize(16);
    validateDrawBuffers(d);
  }

  checkLegacyShaders(nullptr);
  checkMatrixShaders(nullptr);
  fo2CheckMetalFixedShaders(nullptr);
  auto vs = testVS(), ps = testPS();
  for (bool fog : {false, true}) {
    auto literal = testLiteralPS(fog);
    auto entry = literal->source.find("fo2_fragment (");
    auto opening = literal->source.find('(', entry);
    auto first = literal->source.find_first_not_of(" \t\r\n", opening + 1);
    require(entry != std::string::npos && first != std::string::npos &&
                literal->source[first] != ',',
            "Literal-only pixel shader has an invalid leading comma");
    require(literal->floats == 0 && literal->ints == 0 && literal->bools == 0,
            "Literal-only shader unexpectedly requires constants");
  }
  require(vs->inputs.size() == 1 && vs->inputs[0][0] == 0,
          "Vertex reflection failed");
  require(ps->floats == 1, "Pixel constant reflection failed");
  float f[1024]{};
  int32_t i[64]{};
  uint32_t b[16]{};
  f[0] = 0.25;
  f[1] = 0.5;
  f[2] = 0.75;
  f[3] = 1;
  auto packed = packConstants(*ps, f, i, b);
  require(packed.size() == 16 && !std::memcmp(packed.data(), f, 16),
          "Constant packing failed");
  bool invalid = false;
  try {
    translateShader(Bytes{0, 1, 2, 3}, true);
  } catch (const std::exception &) {
    invalid = true;
  }
  require(invalid, "Invalid bytecode accepted");
  auto color = translateShader(
      bytecode({0xfffe0200, 0x0200001f, 0x8000000a, 0x900f0000, 0x02000001,
                0xc00f0000, 0x90e40000, 0x0000ffff}),
      true, 1);
  require(color->source.find("input.v0.zyxw") != std::string::npos,
          "BGRA vertex input swizzle missing");
  std::puts("[Metal check] CPU: shader parsing, reflection, BGRA, constants "
            "and invalid input passed");
}
int checkMetalShaders(const char *directory, bool gpu) {
  cpuChecks();
  auto car=traceCarFixtureDraw();auto carSource=traceCarMaterialSource(*car.vs,*car.ps);
  require(parseWaterReflectionGain(nullptr)==1&&parseWaterReflectionGain("1")==1&&parseWaterReflectionGain("4")==4&&
          parseWaterReflectionGain("999")==8&&parseWaterReflectionGain("-2")==1&&parseWaterReflectionGain("nan")==1&&
          parseWaterReflectionGain("inf")==1&&parseWaterReflectionGain("4junk")==1,"Water reflection gain input bounds failed");
  require(carSource.find("discard_fragment")==std::string::npos&&carSource.find("bias(")==std::string::npos,"RT car source requires derivative-free samples");
  std::filesystem::create_directories("work/metal/msl");std::ofstream("work/metal/msl/trace-car-material.metal")<<carSource;
  std::unique_ptr<Renderer> renderer;
  if (gpu) {
    renderer = std::make_unique<Renderer>();
    fo2CheckMetalFixedShaders(renderer.get());
    renderer->validateShader(testLiteralPS());
    renderer->validateShader(testLiteralPS(true));
    checkLegacyShaders(renderer.get());
    checkMatrixShaders(renderer.get());
  }
  unsigned count = 0, failed = 0;
  if (directory) {
    auto output = std::filesystem::path("work/metal/msl");
    std::filesystem::create_directories(output);
    for (auto &file : std::filesystem::directory_iterator(directory)) {
      if (file.path().extension() != ".bin")
        continue;
      try {
        std::ifstream stream(file.path(), std::ios::binary);
        Bytes bytes((std::istreambuf_iterator<char>(stream)), {});
        if (bytes.size() < 4)
          throw std::runtime_error("Truncated shader");
        uint32_t version;
        std::memcpy(&version, bytes.data(), 4);
        if ((version >> 16) != 0xfffe && (version >> 16) != 0xffff)
          throw std::runtime_error("Unknown shader stage");
        auto shader = translateShader(bytes, (version >> 16) == 0xfffe);
        std::ofstream(output / (file.path().stem().string() + ".metal"))
            << shader->source;
        if (renderer)
          renderer->validateShader(shader);
        if(!shader->vertex){
          if(shaderHash(bytes.data(),bytes.size())==0xf65220e267055268ull){
            auto water=std::make_shared<Shader>(*shader);water->gpu.reset();water->source=waterNormalSource(shader->source);
            water->hash=shaderHash(water->source.data(),water->source.size());if(renderer)renderer->validateShader(water);
          }
          if(shaderHash(bytes.data(),bytes.size())==0xb20adcd636eb7f43ull){
            bool rejected=false;try{waterLegacyWindowSource(shader->source);}catch(const std::exception&){rejected=true;}
            require(rejected,"Water replacement accepted a 2D environment sampler");
            auto physical=translateShader(bytes,false,0,2);require(physical->physicalWaterFamily,"Physical water program identity");
            auto replacement=std::make_shared<Shader>(*physical);replacement->source=waterLegacyWindowSource(physical->source);
            replacement->hash=shaderHash(replacement->source.data(),replacement->source.size());
            std::ofstream(output/"water-replacement.metal")<<replacement->source;
            require(replacement->source.find("r0.xyz *= 1.0-fo2coverage")!=std::string::npos&&
                    replacement->source.find("fo2coverage*=clamp(factor,0.0,1.0)")!=std::string::npos,"Water replacement lost authored base/fog attenuation");
            rejected=false;try{waterLegacyWindowSource(replacement->source);}catch(const std::exception&){rejected=true;}
            require(rejected,"Water replacement accepted duplicate metadata attachment");
            if(renderer)renderer->validateShader(replacement);
            physical->source=waterLegacyZeroSource(physicalWaterNormalSource(physical->source));physical->hash=shaderHash(physical->source.data(),physical->source.size());
            if(renderer)renderer->validateShader(physical);
          }
          if(shader->terrainSpecularMask){
            auto physical=std::make_shared<Shader>(*shader);physical->gpu.reset();physical->source=physicalWaterNormalSource(shader->source,true);
            physical->hash=shaderHash(physical->source.data(),physical->source.size());if(renderer)renderer->validateShader(physical);
          }
          auto variant=std::make_shared<Shader>(*shader);variant->gpu.reset();variant->source=materialOutputSource(shader->source,shader->terrainSpecularMask);
          variant->hash=shaderHash(variant->source.data(),variant->source.size());
          if(renderer)renderer->validateShader(variant);
        }
        ++count;
      } catch (const std::exception &e) {
        std::fprintf(stderr, "[Metal check] %s: %s\n",
                     file.path().filename().c_str(), e.what());
        ++failed;
      }
    }
  }
  std::printf("[Metal check] corpus: %u passed, %u failed%s\n", count, failed,
              gpu ? " (GPU compilation)" : " (CPU translation)");
  return failed ? 1 : 0;
}
static std::shared_ptr<Image> image(unsigned w = 16, unsigned h = 16,
                                    bool depth = false) {
  auto p = std::make_shared<Image>();
  p->width = w;
  p->height = h;
  p->depth = depth;
  if (!depth)
    p->rgba = {Bytes(w * h * 4, 0)};
  return p;
}
static void nearPixel(const Bytes &pixels, unsigned x, unsigned y,
                      std::array<int, 4> expected, const char *text) {
  for (unsigned c = 0; c < 4; ++c)
    if (std::abs(int(pixels[(y * 16 + x) * 4 + c]) - expected[c]) > 2)
      throw std::runtime_error(std::string(text) + " channel " +
                               std::to_string(c) + " got " +
                               std::to_string(pixels[(y * 16 + x) * 4 + c]));
}
static std::shared_ptr<Shader> sourceShader(std::string source,const char* entry,bool vertex=false){
  auto s=std::make_shared<Shader>();s->source=std::move(source);s->entry=entry;s->vertex=vertex;
  s->hash=shaderHash(s->source.data(),s->source.size());return s;
}
static void checkTerrain(Renderer& renderer){
  Draw d;d.color=image();d.materialTarget=image();d.count=3;
  d.raster.viewport[2]=d.raster.viewport[3]=16;d.raster.depthOn=d.raster.depthWrite=false;
  d.vs=sourceShader(R"MSL(#include <metal_stdlib>
using namespace metal;
struct TerrainTestOut{float4 position [[position]];float4 t0 [[user(texcoord0)]];float4 t1 [[user(texcoord1)]];float4 v0 [[user(color0)]];};
vertex TerrainTestOut terrainTestVS(uint i [[vertex_id]]){
return {float4(i==1?3.0:-1.0,i==2?3.0:-1.0,0,1),float4(.5),float4(.5),float4(.2)};}
)MSL","terrainTestVS",true);
  auto original=translateShader(terrainBytecode(),false);
  auto variant=sourceShader(materialOutputSource(original->source,true),"fo2_fragment");
  variant->samplerMask=original->samplerMask;
  d.textures[0]=image(1,1);d.textures[1]=image(1,1);
  d.effects[24]=terrainSpecularMaterial().reflectivity;d.effects[25]=terrainSpecularMaterial().roughness;
  d.effects[26]=float(unsigned(MaterialCue::TerrainSpecular))/255;
  auto mask=d.materialTarget;float zero[4]{};
  for(auto alpha: {std::array<uint8_t,2>{0,255},{255,0},{128,128},{255,255}}){
    d.textures[0]->rgba[0]={128,64,32,alpha[0]};++d.textures[0]->version;
    d.textures[1]->rgba[0]={255,128,255,alpha[1]};++d.textures[1]->version;
    d.materialTarget.reset();d.ps=original;renderer.clear(d.color,{},1,zero,1,0);renderer.draw(d);
    auto color=renderer.read(d.color);
    d.materialTarget=mask;d.ps=variant;renderer.clear(d.color,{},1,zero,1,0);renderer.clear(mask,{},1,zero,1,0);renderer.draw(d);
    require(renderer.read(d.color)==color,"Terrain MRT changed original color/alpha");
    float coverage=float(alpha[0])*alpha[1]/(255.f*255.f);
    nearPixel(renderer.read(mask),8,8,{int(std::lround(.04f*coverage*255)),
      int(std::lround((.9f+(terrainSpecularMaterial().roughness-.9f)*coverage)*255)),4,int(std::lround(coverage*255))},"Terrain authored alpha-product mask");
  }
  d.effects[0]=1;d.effects[1]=5;d.effects[2]=1;
  renderer.clear(mask,{},1,zero,1,0);renderer.draw(d);
  nearPixel(renderer.read(mask),8,8,{0,0,0,0},"Terrain alpha-discard must not write material mask");
  std::puts("[Metal check] terrain: dry/partial/wet alpha product, roughness, original color and alpha-discard preservation passed");
}
static void checkRoughReflections(Renderer& renderer,const Draw& fixture){
  Draw d=fixture;d.color=image();d.depth.reset();d.debugProjection={};d.raster.depthOn=false;
  d.ps=sourceShader(std::string(debugViewSource)+ssrSource+R"MSL(
fragment float4 roughFilterTestPS(float4 p [[position]],texture2d<float> scene [[texture(0)]],
 depth2d<float,access::read> depth [[texture(1)]],constant float4* c [[buffer(0)]]){
return float4(ssrRoughRadiance(scene,depth,p.xy/16.0,c[1].x,c),1);}
)MSL","roughFilterTestPS");d.ps->samplerMask=3;
  auto scene=image(),depth=image(16,16,true);d.textures[0]=scene;d.textures[1]=depth;
  for(unsigned i=0;i<256;++i)scene->rgba[0][i*4+3]=255;
  for(unsigned i=0;i<3;++i)scene->rgba[0][(8*16+8)*4+i]=255;
  float c[8]={1,1,1.01f,-.1f,0,0,0,0};float zero[4]{};
  auto render=[&](float rough){c[4]=rough;d.psConstants.resize(sizeof c);std::memcpy(d.psConstants.data(),c,sizeof c);
    renderer.clear(d.color,{},1,zero,1,0);renderer.draw(d);return renderer.read(d.color);};
  renderer.clear({},depth,2,zero,.5f,0);
  nearPixel(render(0),8,8,{255,255,255,255},"Smooth SSR radiance changed");
  nearPixel(render(.5f),8,8,{85,85,85,255},"Rough SSR radiance not filtered");
  renderer.clear({},depth,2,zero,.8f,0);
  uint32_t center[4]={8,8,1,1};renderer.clear(d.color,depth,2,zero,.5f,0,center);
  nearPixel(render(.5f),8,8,{255,255,255,255},"Rough SSR filter crossed a depth discontinuity");
  std::puts("[Metal check] rough SSR: sharp/rough radiance and depth-edge rejection passed");
}
static void checkPhysicalPuddleMask(Renderer& renderer){
  auto program=bytecode({0xffff0101,0x42,0xb00f0000,0x42,0xb00f0001,0x5,0x81170000,0xb0e40000,0x90e40000,
    0x40000005,0x80080000,0xb0ff0000,0x90ff0000,0x12,0x80070000,0x80ff0000,0xb0e40001,0x80e40000,
    0x5,0x80080001,0xb0ff0000,0xb0ff0001,0x4,0x80170000,0x90e40001,0x80ff0001,0x80e40000,
    0x40000002,0x80180000,0x80ff0000,0x80ff0001,0xffff});
  for(bool terrain:{false,true}){
  auto original=translateShader(terrain?terrainBytecode():program,false,0,terrain?0:2);require(original->physicalWaterFamily==(terrain?2:1),"Physical puddle pixel program identity");
  auto normal=std::make_shared<Shader>(*original);normal->source=physicalWaterNormalSource(original->source,terrain);normal->hash=shaderHash(normal->source.data(),normal->source.size());
  Draw d;d.color=image();d.count=3;d.raster.depthOn=d.raster.depthWrite=false;d.raster.viewport[2]=d.raster.viewport[3]=16;
  d.vs=sourceShader(R"MSL(#include <metal_stdlib>
using namespace metal;
struct PuddleOut{float4 position [[position]];float4 uv [[user(texcoord0)]];float4 reflection [[user(texcoord1)]];float4 diffuse [[user(color0)]];float4 specular [[user(color1)]];};
vertex PuddleOut puddleVS(uint i [[vertex_id]]){return {float4(i==1?3.0:-1.0,i==2?3.0:-1.0,0,1),float4(.5),float4(1,0,0,1),float4(1),float4(.1)};}
)MSL","puddleVS",true);
  d.textures[0]=image(1,1);d.textures[1]=image(1,1);d.textures[1]->faces=terrain?1:6;d.textures[1]->rgba.assign(terrain?1:6,Bytes{128,128,128,255});
  d.effects[48]=0;d.effects[49]=1;d.effects[50]=0;float zero[4]{};
  for(uint8_t alpha:{uint8_t(0),uint8_t(128),uint8_t(255)}){
    d.textures[0]->rgba[0]={128,128,128,alpha};++d.textures[0]->version;
    d.ps=original;renderer.clear(d.color,{},1,zero,1,0);renderer.draw(d);auto colorBefore=renderer.read(d.color);
    d.ps=normal;renderer.clear(d.color,{},1,zero,1,0);renderer.draw(d);
    nearPixel(renderer.read(d.color),8,8,alpha?std::array<int,4>{128,255,128,int(alpha)}:std::array<int,4>{0,0,0,0},"Physical puddle normal/authored-alpha mask");
    if(!terrain){
      auto replacement=std::make_shared<Shader>(*original);replacement->source=waterLegacyWindowSource(original->source);
      replacement->hash=shaderHash(replacement->source.data(),replacement->source.size());
      d.ps=replacement;d.materialTarget=image();renderer.clear(d.materialTarget,{},1,zero,1,0);
      renderer.clear(d.color,{},1,zero,1,0);renderer.draw(d);
      // Base is 1.0; the old cube is 128/255. Removing just the cube
      // retains base*(1-alpha), not the unattenuated base.
      int encoded=int(std::lround((128.0/255*alpha/255*.5+.5)*255));
      nearPixel(renderer.read(d.materialTarget),8,8,alpha?std::array<int,4>{encoded,encoded,encoded,int(alpha)}:std::array<int,4>{0,0,0,0},"Water authored environment delta/coverage");
      if(alpha==255){
        d.effects[3]=1;d.effects[4]=3;d.effects[5]=0;d.effects[6]=2;d.effects[8]=d.effects[9]=d.effects[10]=.2f;
        renderer.draw(d);nearPixel(renderer.read(d.materialTarget),8,8,{160,160,160,128},"Water replacement lost fog attenuation/direct light");d.effects[3]=0;
      }
      auto reset=std::make_shared<Shader>(*normal);reset->source=waterLegacyZeroSource(normal->source);reset->hash=shaderHash(reset->source.data(),reset->source.size());
      if(alpha){d.ps=reset;renderer.draw(d);nearPixel(renderer.read(d.materialTarget),8,8,{128,128,128,0},"Unsupported water did not invalidate old metadata");}
      d.materialTarget.reset();
    }
    d.ps=original;renderer.clear(d.color,{},1,zero,1,0);renderer.draw(d);
    require(renderer.read(d.color)==colorBefore,"Private puddle mask changed original shader output");
  }
  }
  std::puts("[Metal check] physical puddle: verified static window/terrain programs, dry/partial/wet authored alpha, view normal and original-color preservation passed");
}
static void checkHybridWaterComposite(Renderer& renderer,const Draw& fixture){
  Draw d=fixture;d.color=image();d.depth.reset();d.debugProjection={};d.raster.depthOn=d.raster.depthWrite=false;
  d.ps=sourceShader(std::string(debugViewSource)+ssrSource,"ssrCompositePS");d.ps->samplerMask=2047;
  auto base=image(),scene=image(),materials=image(),normals=image(),ssr=image(8,8),rt=image(4,4);
  auto opaque=image(16,16,true),water=image(16,16,true),legacy=image(),authored=image();
  auto solid=[](const std::shared_ptr<Image>& image,std::array<uint8_t,4> rgba){
    for(size_t i=0;i<image->rgba[0].size();i+=4)std::copy(rgba.begin(),rgba.end(),image->rgba[0].begin()+i);++image->version;};
  solid(base,{0,0,255,255});solid(scene,{0,0,255,255});solid(normals,{128,128,255,255});
  solid(ssr,{0,255,0,128});solid(rt,{255,0,0,128});
  d.textures[0]=base;d.textures[1]=scene;d.textures[2]=ssr;d.textures[3]=materials;d.textures[4]=normals;
  d.textures[5]=scene;d.textures[6]=opaque;d.textures[7]=water;d.textures[8]=rt;d.textures[9]=legacy;d.textures[10]=authored;
  float c[16]={1,1,1.01f,-.1f,0,0,0,0,8,8,0,1,1,.04f,.1f,3};float zero[4]{};
  renderer.clear({},opaque,2,zero,.8f,0);renderer.clear({},water,2,zero,.5f,0);
  auto render=[&]{d.psConstants.resize(sizeof c);std::memcpy(d.psConstants.data(),c,sizeof c);renderer.draw(d);return renderer.read(d.color);};
  nearPixel(render(),8,8,{0,128,127,255},"Disabled water RT changed SSR");
  c[4]=1;nearPixel(render(),8,8,{128,0,127,255},"Water RT did not replace valid water radiance");
  c[5]=4;solid(rt,{255,0,0,16});nearPixel(render(),8,8,{64,0,191,255},"Water diagnostic gain did not reveal a weak RT reflection");
  c[5]=1;nearPixel(render(),8,8,{16,0,239,255},"Unit water gain changed original reflection strength");
  c[5]=4;solid(rt,{255,0,0,128});nearPixel(render(),8,8,{255,0,0,255},"Water reflection gain did not clamp safely");
  c[5]=0;
  solid(rt,{255,0,0,0});nearPixel(render(),8,8,{0,128,127,255},"Water RT miss lost SSR fallback");
  c[5]=4;solid(rt,{255,0,0,128});solid(base,{255,255,0,255});nearPixel(render(),8,8,{255,255,0,255},"Water RT overwrote HUD");
  solid(base,{0,0,255,255});solid(normals,{128,128,255,0});nearPixel(render(),8,8,{0,0,255,255},"Water RT leaked onto nonwater");
  // Original authored color = retained base/direct light + old environment.
  // Replace only the environment contribution, without a second Fresnel blend.
  c[5]=1;solid(normals,{128,128,255,255});solid(base,{128,128,128,255});solid(scene,{128,128,128,255});
  solid(authored,{128,128,128,255});solid(legacy,{160,160,160,128});solid(rt,{255,0,0,128});
  nearPixel(render(),8,8,{191,63,63,255},"Authored water environment was not replaced exactly once");
  solid(rt,{255,0,0,0});solid(ssr,{0,255,0,0});nearPixel(render(),8,8,{128,128,128,255},"Replacement removed original environment on an RT miss");
  solid(rt,{255,0,0,128});solid(base,{255,255,0,255});nearPixel(render(),8,8,{255,255,0,255},"Replacement overwrote a HUD overlay");
  solid(base,{128,128,128,255});solid(authored,{0,0,0,255});
  nearPixel(render(),8,8,{192,64,64,255},"Replacement ignored authored/late-scene mismatch");
  solid(authored,{128,128,128,255});solid(normals,{128,128,255,0});
  nearPixel(render(),8,8,{128,128,128,255},"Replacement leaked onto nonwater");
  d.color=image(4,4);d.raster.viewport[2]=d.raster.viewport[3]=4;
  d.ps=sourceShader(std::string(debugViewSource)+ssrSource,"reflectionCopyPS");d.ps->samplerMask=1;d.textures[0]=rt;
  renderer.draw(d);auto copied=renderer.read(d.color);
  for(size_t i=0;i<copied.size();i+=4)require(copied[i]==255&&copied[i+1]==0&&copied[i+2]==0&&copied[i+3]==128,"RT reflection inspection conversion changed radiance/confidence");
  std::puts("[Metal check] hybrid water composite: on/off, SSR fallback, HUD and nonwater preservation passed");
}
static void checkReflectionResolve(Renderer& renderer,const Draw& fixture){
  Draw d=fixture;d.color=image();d.depth.reset();d.debugProjection={};d.raster.depthOn=d.raster.depthWrite=false;
  d.ps=sourceShader(std::string(debugViewSource)+ssrSource+R"MSL(
fragment float4 resolveTestPS(float4 p [[position]],texture2d<float> hits [[texture(0)]],
 texture2d<float> materials [[texture(1)]],texture2d<float> normals [[texture(2)]],
 depth2d<float,access::read> depth [[texture(3)]],depth2d<float,access::read> water [[texture(4)]],
 constant float4* c [[buffer(0)]]){
 if(c[1].x>.5)return float4(float3(water.read(uint2(p.xy))),1);
 return ssrResolve(uint2(p.xy),hits,materials,normals,depth,water,c);}
)MSL","resolveTestPS");d.ps->samplerMask=31;
  auto hits=image(8,8),materials=image(),normals=image(),depth=image(16,16,true),water=image(16,16,true);
  d.textures[0]=hits;d.textures[1]=materials;d.textures[2]=normals;d.textures[3]=depth;d.textures[4]=water;
  float c[16]={1,1,1.01f,-.1f,0,0,0,0,8,8,0,0,1,.02f,.1f,3};
  d.psConstants.resize(sizeof c);std::memcpy(d.psConstants.data(),c,sizeof c);
  float zero[4]{};renderer.clear({},depth,2,zero,.8f,0);renderer.clear({},water,2,zero,.5f,0);
  for(unsigned i=0;i<256;++i){auto& n=normals->rgba[0];n[i*4]=n[i*4+1]=128;n[i*4+2]=n[i*4+3]=255;}
  auto fillHits=[&](bool sparse){std::fill(hits->rgba[0].begin(),hits->rgba[0].end(),0);
    for(unsigned i=0;i<64;++i)if(!sparse||i==4*8+4){hits->rgba[0][i*4]=255;hits->rgba[0][i*4+3]=128;}++hits->version;};
  auto render=[&](){renderer.clear(d.color,{},1,zero,1,0);renderer.draw(d);return renderer.read(d.color);};
  fillHits(false);nearPixel(render(),8,8,{255,0,0,128},"SSR resolve changed uniform radiance/confidence");
  fillHits(true);nearPixel(render(),8,8,{255,0,0,72},"SSR hit/miss boundary darkened radiance");
  fillHits(false);auto index=(4*8+4)*4;hits->rgba[0][index]=0;hits->rgba[0][index+2]=255;++hits->version;
  normals->rgba[0][(9*16+9)*4+3]=0;++normals->version;
  nearPixel(render(),8,8,{255,0,0,128},"SSR resolve leaked a nonwater neighbor onto water");
  normals->rgba[0][(9*16+9)*4+3]=255;++normals->version;
  // Rectangular clears use a draw and require a color attachment, even when
  // its write mask is zero. Verify the setup before testing depth rejection.
  uint32_t rect[4]={9,9,1,1};renderer.clear(d.color,water,2,zero,.2f,0,rect);
  c[4]=1;std::memcpy(d.psConstants.data(),c,sizeof c);
  auto depthProbe=render();
  nearPixel(depthProbe,9,9,{51,51,51,255},"SSR resolve fixture did not create the depth step");
  nearPixel(depthProbe,8,8,{128,128,128,255},"SSR resolve fixture changed depth outside its rectangle");
  c[4]=0;std::memcpy(d.psConstants.data(),c,sizeof c);
  nearPixel(render(),8,8,{255,0,0,128},"SSR resolve crossed a receiver depth step");
  renderer.clear({},water,2,zero,.5f,0);
  normals->rgba[0][(9*16+9)*4]=255;normals->rgba[0][(9*16+9)*4+2]=128;++normals->version;
  nearPixel(render(),8,8,{255,0,0,128},"SSR resolve crossed a water-normal discontinuity");
  std::fill(hits->rgba[0].begin(),hits->rgba[0].end(),0);++hits->version;
  nearPixel(render(),8,8,{0,0,0,0},"SSR resolve invented a hit in an empty neighborhood");
  c[12]=0;c[11]=1;std::memcpy(d.psConstants.data(),c,sizeof c);
  for(unsigned i=0;i<256;++i){materials->rgba[0][i*4]=128;materials->rgba[0][i*4+2]=1;materials->rgba[0][i*4+3]=255;}
  materials->rgba[0][(9*16+9)*4+2]=2;++materials->version;
  fillHits(false);hits->rgba[0][index]=0;hits->rgba[0][index+2]=255;++hits->version;
  nearPixel(render(),8,8,{255,0,0,128},"SSR resolve leaked across opaque material classes");
  materials->rgba[0][(8*16+8)*4]=0;++materials->version;
  nearPixel(render(),8,8,{0,0,0,0},"SSR resolve applied reflection to an unclassified receiver");
  std::puts("[Metal check] SSR resolve: confidence-weighted radiance, water/depth/normal/material boundaries and empty neighborhoods passed");
}
static void checkSSR(Renderer& renderer,const Draw& fixture,bool materials=false,bool water=false,bool lateScene=false){
  Draw d=fixture;d.color=image();d.depth=image(16,16,true);d.count=6;
  d.captureMaterial=true;d.streams[0]=std::make_shared<Buffer>();d.debugProjection={{1,1,1.01f,-.1f},true};
  d.raster.depthOn=d.raster.depthWrite=true;d.raster.cull=0;
  renderer.setDebugCapture(false);renderer.setDebugCapture(true,true,materials);
  float black[4]={0,0,0,1};renderer.clear(d.color,d.depth,3,black,1,0);
  auto quad=[&](const std::array<std::array<float,3>,6>& points,std::array<float,4> color,MaterialHint material=MaterialHint{}){
    d.material=material;
    float vertices[24];
    for(unsigned i=0;i<6;++i){float distance=-points[i][2];vertices[i*4]=points[i][0];
      vertices[i*4+1]=points[i][1];vertices[i*4+2]=1.01f*distance-.1f;vertices[i*4+3]=distance;}
    d.streams[0]->bytes.resize(sizeof vertices);std::memcpy(d.streams[0]->bytes.data(),vertices,sizeof vertices);++d.streams[0]->version;
    float f[1024]{};std::copy(color.begin(),color.end(),f);int32_t ints[64]{};uint32_t bools[16]{};
    d.psConstants=packConstants(*d.ps,f,ints,bools);renderer.draw(d);
  };
  quad({{{-8,-1,-1.1f},{8,-1,-1.1f},{-8,-1,-8},{8,-1,-1.1f},{8,-1,-8},{-8,-1,-8}}},{.1f,.2f,.3f,1},water?MaterialHint{}:classifyMaterial(0,false,true,.5f,.5f,.5f,30));
  quad({{{-8,-1,-5},{8,-1,-5},{-8,4,-5},{8,-1,-5},{8,4,-5},{-8,4,-5}}},{1,0,0,1});
  if(materials){
    quad({{{-8,-.9f,-1.1f},{0,-.9f,-1.1f},{-8,-.9f,-8},{0,-.9f,-1.1f},{0,-.9f,-8},{-8,-.9f,-8}}},{.1f,.2f,.3f,1});
    d.effects[0]=1;d.effects[1]=5;d.effects[2]=.5f;
    quad({{{0,-.9f,-1.1f},{8,-.9f,-1.1f},{0,-.9f,-8},{8,-.9f,-1.1f},{8,-.9f,-8},{0,-.9f,-8}}},{.1f,.2f,.3f,0});
  }
  if(water){
    auto colorPS=std::make_shared<Shader>(*d.ps);colorPS->waterVariant=std::make_shared<Shader>();
    auto normal=colorPS->waterVariant;
    normal->source="#include <metal_stdlib>\nusing namespace metal;fragment float4 waterReflectionTestPS(){return float4(.5,1,.5,1);}";
    normal->entry="waterReflectionTestPS";normal->hash=shaderHash(normal->source.data(),normal->source.size());d.ps=colorPS;
    d.effects[0]=0;d.waterBasisValid=true;d.waterBasis={1,0,0,0,1,0,0,0,1};
    d.raster.depthWrite=false;d.raster.blendOn=true;d.raster.src=Blend::One;d.raster.dst=Blend::InvSrcAlpha;
    quad({{{-8,-.95f,-1.1f},{8,-.95f,-1.1f},{-8,-.95f,-8},{8,-.95f,-1.1f},{8,-.95f,-8},{-8,-.95f,-8}}},{0,.1f,.2f,.5f},worldWaterMaterial());
  }
  if(lateScene){
    // Simulate lighting/color work after the early water pass. It changes the
    // background substantially but does not belong to the HUD.
    auto processed=image();processed->rgba[0]=renderer.read(d.color);
    for(size_t i=0;i<processed->rgba[0].size();i+=4)for(unsigned k=0;k<3;++k)
      processed->rgba[0][i+k]=uint8_t(102+processed->rgba[0][i+k]*.6f);
    renderer.copy(processed,d.color);
    Draw hud=d;hud.hudBoundary=true;hud.captureMaterial=false;hud.waterBasisValid=false;hud.material={};hud.debugProjection={};
    hud.raster.depthOn=hud.raster.depthWrite=false;hud.raster.blendOn=true;hud.raster.src=Blend::SrcAlpha;hud.raster.dst=Blend::InvSrcAlpha;
    float transparent[1024]{};int32_t ints[64]{};uint32_t bools[16]{};
    hud.psConstants=packConstants(*hud.ps,transparent,ints,bools);renderer.draw(hud);
  }
  auto original=renderer.read(d.color);
  require(renderer.screenReflections(d.color,false,false)==d.color,"Disabled SSR changed the output image");
  auto reflected=renderer.screenReflections(d.color);
  require(bool(reflected),"SSR scene capture unavailable");
  auto pixels=renderer.read(reflected);unsigned hits=0;
  static unsigned inspectionFixture=0;
  const char* inspectionSession=std::getenv("FO2_METAL_SESSION_DIR");
  auto inspectionFolder=(inspectionSession?std::filesystem::path(inspectionSession):std::filesystem::path("work/metal/ssr-check"))/("inspection-"+std::to_string(inspectionFixture++));
  require(renderer.captureReflectionDiagnostics(inspectionFolder.string(),d.color),"SSR diagnostic capture failed");
  require(renderer.read(d.color)==original&&renderer.read(reflected)==pixels,"SSR diagnostic mutated game/reflection output");
  bool depthChecked=false;
  for(const auto& slot:std::filesystem::directory_iterator(inspectionFolder)){
    auto path=slot.path()/"opaque-depth.f32";if(!std::filesystem::exists(path))continue;
    std::ifstream file(path,std::ios::binary);Bytes raw((std::istreambuf_iterator<char>(file)),{});
    require(raw.size()==16*16*4,"SSR inspection raw depth size mismatch");
    for(size_t offset=0;offset<raw.size();offset+=4){float z;std::memcpy(&z,raw.data()+offset,4);
      require(std::isfinite(z)&&z>=0&&z<=1,"SSR inspection raw depth invalid");}
    float sky;std::memcpy(&sky,raw.data(),4);require(sky==1,"SSR inspection raw depth byte order/precision mismatch");depthChecked=true;
  }
  require(depthChecked,"SSR inspection raw depth file missing");

  for(unsigned y=10;y<15;++y)for(unsigned x=2;x<14;++x){unsigned i=(y*16+x)*4;
    if(pixels[i]>original[i]+3 && pixels[i+2]<original[i+2])++hits;}
  auto mask=renderer.screenReflections(d.color,true);require(bool(mask),"SSR receiver mask unavailable");
  auto maskPixels=renderer.read(mask);
  const char* session=std::getenv("FO2_METAL_SESSION_DIR");
  auto folder=session?std::filesystem::path(session)/"gpu-check":std::filesystem::path("work/metal/ssr-check");
  std::filesystem::create_directories(folder);
  auto save=[&](const char* name,const Bytes& bytes){std::ofstream file(folder/name,std::ios::binary);file<<"P6\n16 16\n255\n";
    for(size_t i=0;i<bytes.size();i+=4)file.write(reinterpret_cast<const char*>(bytes.data()+i),3);};
  save(water?"water-scene.ppm":materials?"material-scene.ppm":"scene.ppm",original);save(water?"water-ssr.ppm":materials?"material-ssr.ppm":"ssr.ppm",pixels);save(water?"water-mask.ppm":materials?"material-mask.ppm":"mask.ppm",maskPixels);
  std::printf("[Metal check] SSR diagnostic images: %s; floor hits=%u\n",folder.string().c_str(),hits);
  require(hits>0,"SSR did not reflect the red wall onto the blue floor");
  nearPixel(maskPixels,12,12,water?std::array<int,4>{0,153,204,255}:materials?std::array<int,4>{128,128,128,255}:std::array<int,4>{255,255,255,255},"SSR floor receiver mask / alpha-discard preservation");
  if(materials)nearPixel(maskPixels,4,12,{0,0,0,255},"Unknown opaque material occludes reflective surface");
  // A later UI overlay differs from the captured scene and must stay intact.
  float hud[4]={1,0,1,1};renderer.clear(d.color,d.depth,1,hud,1,0);
  nearPixel(renderer.read(renderer.screenReflections(d.color)),8,12,{255,0,255,255},"SSR HUD preservation");
  renderer.setDebugCapture(false);renderer.setDebugCapture(true,true,materials);
  renderer.clear(d.color,d.depth,3,black,1,0);
  auto noScene=renderer.screenReflections(d.color);
  require(!noScene,"SSR accepted a cleared frame without scene geometry");
  renderer.setDebugCapture(false);
  std::printf("[Metal check] SSR: %u floor hits, receiver mask, HUD preservation and missing-scene rejection passed%s\n",hits,lateScene?" (late pre-HUD lighting reference)":"");
}
static void checkWater(Renderer& renderer,const Draw& fixture,bool stencil=false){
  Draw d=fixture;d.color=image();d.depth=image(16,16,true);d.depth->stencil=stencil;d.captureMaterial=true;
  d.streams[0]=std::make_shared<Buffer>();d.debugProjection={{1,1,1.01f,-.1f},true};
  d.material={};d.raster.depthOn=d.raster.depthWrite=true;d.raster.blendOn=false;
  renderer.setDebugCapture(false);renderer.setDebugCapture(true,true,true);
  float black[4]={0,0,0,1};renderer.clear(d.color,d.depth,stencil?7:3,black,1,0);
  auto geometry=[&](std::initializer_list<std::array<float,4>> points,std::array<float,4> color){
    d.count=points.size();d.streams[0]->bytes.resize(points.size()*16);unsigned n=0;
    for(const auto& point:points){std::memcpy(d.streams[0]->bytes.data()+n*16,point.data(),16);++n;}++d.streams[0]->version;
    float f[1024]{};std::copy(color.begin(),color.end(),f);int32_t ints[64]{};uint32_t bools[16]{};
    d.psConstants=packConstants(*d.ps,f,ints,bools);renderer.draw(d);
  };
  geometry({{-1,-1,.9f,1},{3,-1,.9f,1},{-1,3,.9f,1}},{.1f,.2f,.3f,1});
  geometry({{-.75f,.25f,.4f,1},{-.25f,.25f,.4f,1},{-.75f,.75f,.4f,1},
            {-.25f,.25f,.4f,1},{-.25f,.75f,.4f,1},{-.75f,.75f,.4f,1}},{0,1,0,1});
  auto waterPS=std::make_shared<Shader>(*d.ps);waterPS->waterVariant=std::make_shared<Shader>();
  waterPS->waterVariant->source="#include <metal_stdlib>\nusing namespace metal;fragment float4 waterNormalTestPS(){return float4(.5,1,.5,1);}";
  waterPS->waterVariant->entry="waterNormalTestPS";waterPS->waterVariant->hash=shaderHash(waterPS->waterVariant->source.data(),waterPS->waterVariant->source.size());
  d.ps=waterPS;d.material=worldWaterMaterial();d.waterBasisValid=true;d.waterBasis={1,0,0,0,1,0,0,0,1};
  d.raster.depthWrite=false;d.raster.blendOn=true;d.raster.src=Blend::One;d.raster.dst=Blend::InvSrcAlpha;
  geometry({{-1,-1,.6f,1},{0,-1,.6f,1},{-1,1,.6f,1},{0,-1,.6f,1},{0,1,.6f,1},{-1,1,.6f,1}},{0,.1f,.2f,.5f});
  d.ps=fixture.ps;d.material={};d.waterBasisValid=false;d.raster.depthWrite=true;d.raster.blendOn=false;
  geometry({{-.75f,-.75f,.3f,1},{-.25f,-.75f,.3f,1},{-.75f,-.25f,.3f,1},
            {-.25f,-.75f,.3f,1},{-.25f,-.25f,.3f,1},{-.75f,-.25f,.3f,1}},{0,1,0,1});
  auto mask=renderer.screenReflections(d.color,true);require(bool(mask),"Water receiver capture unavailable");
  auto pixels=renderer.read(mask);
  nearPixel(pixels,7,12,{0,153,204,255},"Water surface included without a game depth write");
  nearPixel(pixels,4,4,{0,0,0,255},"Opaque foreground occludes water");
  nearPixel(pixels,4,12,{0,0,0,255},"Opaque geometry drawn after water still occludes its SSR layer");
  nearPixel(pixels,12,12,{0,0,0,255},"Water capture escaped its geometry");
  float hud[4]={1,0,1,1};renderer.clear(d.color,d.depth,1,hud,1,0);
  nearPixel(renderer.read(renderer.screenReflections(d.color)),7,12,{255,0,255,255},"HUD above water preserved");
  renderer.setDebugCapture(false);d.material={};d.waterBasisValid=false;d.captureMaterial=false;d.raster.blendOn=false;
  geometry({{-1,-1,.75f,1},{3,-1,.75f,1},{-1,3,.75f,1}},{1,0,0,1});
  nearPixel(renderer.read(d.color),7,12,{255,0,0,255},"Water capture must not modify original game depth");
  std::puts("[Metal check] water: separate receiver depth/normal, opaque occlusion, bounded coverage, HUD and untouched game depth passed");
}
int checkMetalRenderer() {
  cpuChecks();
  Renderer renderer;
  fo2CheckMetalFixedShaders(&renderer);
  renderer.validateShader(testLiteralPS());
  renderer.validateShader(testLiteralPS(true));
  checkLegacyShaders(&renderer);
  checkMatrixShaders(&renderer);
  Draw d;
  d.color = image();
  d.depth = image(16, 16, true);
  d.vs = testVS();
  d.ps = testPS();
  d.attributes = {{0, 0, 0, Element::Float4}};
  d.strides[0] = 16;
  d.streams[0] = std::make_shared<Buffer>();
  float vertices[] = {-1, -1, 0.5, 1, 3, -1, 0.5, 1, -1, 3, 0.5, 1};
  d.streams[0]->bytes.resize(sizeof(vertices));
  std::memcpy(d.streams[0]->bytes.data(), vertices, sizeof(vertices));
  d.count = 3;
  d.raster.viewport[2] = d.raster.viewport[3] = 16;
  d.raster.cull = 0;
  float constants[1024]{};
  int32_t ints[64]{};
  uint32_t bools[16]{};
  constants[0] = 0.25;
  constants[1] = 0.5;
  constants[2] = 0.75;
  constants[3] = 1;
  d.psConstants = packConstants(*d.ps, constants, ints, bools);
  float black[] = {0, 0, 0, 0};
  renderer.clear(d.color, d.depth, 3, black, 1, 0);
  renderer.draw(d);
  nearPixel(renderer.read(d.color), 8, 8, {64, 128, 191, 255}, "Triangle draw");
  {
    auto before=renderer.read(d.color);Draw hidden=d;hidden.rayOnly=true;
    hidden.streams[0]=std::make_shared<Buffer>(*d.streams[0]);hidden.streams[0]->gpu.reset();
    float z=.25f;for(unsigned i=0;i<3;++i)std::memcpy(hidden.streams[0]->bytes.data()+i*16+8,&z,4);
    float red[1024]={1,0,0,1};hidden.psConstants=packConstants(*hidden.ps,red,ints,bools);
    renderer.draw(hidden);require(renderer.read(d.color)==before,"Ray-only draw changed ordinary color");
    // A .4-depth ordinary draw must still pass the original .5 depth. If the
    // hidden .25 draw wrote depth, this visible green probe would fail.
    Draw probe=hidden;probe.rayOnly=false;probe.streams[0]=std::make_shared<Buffer>(*hidden.streams[0]);probe.streams[0]->gpu.reset();
    z=.4f;for(unsigned i=0;i<3;++i)std::memcpy(probe.streams[0]->bytes.data()+i*16+8,&z,4);
    float green[1024]={0,1,0,1};probe.psConstants=packConstants(*probe.ps,green,ints,bools);renderer.draw(probe);
    nearPixel(renderer.read(d.color),8,8,{0,255,0,255},"Ray-only depth isolation and following visible draw");
    std::puts("[Metal check] ray-only draw: ordinary color/depth preserved; subsequent visible draw passed");
  }
  // Capture before the game clears its scene depth for HUD/postprocessing.
  renderer.resize(16,16);
  for(bool stencil : {false,true}){
    renderer.setDebugCapture(false);renderer.setDebugCapture(true);
    d.depth=image(16,16,true);d.depth->stencil=stencil;
    d.debugProjection={{1,1,1.1f,-.1f},true};
    renderer.clear(d.color,d.depth,3,black,1,0);renderer.draw(d);
    renderer.clear(d.color,d.depth,2,black,1,0);
    auto linear=renderer.debugView(DebugView::Depth,1);
    require(bool(linear),"Depth debug capture unavailable");
    nearPixel(renderer.read(linear),8,8,{213,213,213,255},"Linear depth snapshot");
    auto normal=renderer.debugView(DebugView::Normals);
    require(bool(normal),"Normal debug capture unavailable");
    nearPixel(renderer.read(normal),8,8,{128,128,255,255},"Depth reconstructed normal");
  }
  renderer.setDebugCapture(false);
  const auto mode=parseDebugView(std::getenv("FO2_DEBUG_VIEW"));
  if(mode==DebugView::SSR||mode==DebugView::SSRMask){checkSSR(renderer,d);checkSSR(renderer,d,true);checkSSR(renderer,d,true,true);checkSSR(renderer,d,true,true,true);checkWater(renderer,d);checkWater(renderer,d,true);checkTerrain(renderer);checkRoughReflections(renderer,d);checkReflectionResolve(renderer,d);checkHybridWaterComposite(renderer,d);checkPhysicalPuddleMask(renderer);}
  d.debugProjection={};d.depth=image(16,16,true);
  renderer.clear(d.color,d.depth,3,black,1,0);renderer.draw(d);
  // Relative constant matrices must render identically with an aliased vector.
  auto originalVS = d.vs;
  for (unsigned op : {20u, 21u, 22u, 23u, 24u})
    for (bool alias : {false, true}) {
      d.vs = testMatrixVS(op, alias);
      float vf[1024]{};
      vf[37 * 4] = vf[38 * 4 + 1] = vf[39 * 4 + 2] = 1;
      vf[40 * 4 + 2] = op == 22 ? 2 : 0;
      vf[40 * 4 + 3] = op == 22 ? 0 : 1;
      d.vsConstants = packConstants(*d.vs, vf, ints, bools);
      renderer.clear(d.color, d.depth, 3, black, 1, 0);
      renderer.draw(d);
      nearPixel(renderer.read(d.color), 8, 8, {64, 128, 191, 255},
                "Relative/aliased vertex matrix");
    }
  d.vs = originalVS;
  d.vsConstants.clear();
  // Depth must reject a second fragment behind the stored 0.5 depth.
  for (unsigned v = 0; v < 3; ++v)
    vertices[v * 4 + 2] = 0.8;
  std::memcpy(d.streams[0]->bytes.data(), vertices, sizeof(vertices));
  ++d.streams[0]->version;
  constants[0] = 1;
  constants[1] = constants[2] = 0;
  d.psConstants = packConstants(*d.ps, constants, ints, bools);
  renderer.draw(d);
  nearPixel(renderer.read(d.color), 8, 8, {64, 128, 191, 255},
            "Depth rejection");
  d.raster.depthOn = false;
  d.raster.colorMask = 1;
  renderer.draw(d);
  nearPixel(renderer.read(d.color), 8, 8, {255, 128, 191, 255},
            "RGBA write mask");
  d.raster.colorMask = 15;
  d.raster.scissorOn = true;
  d.raster.scissor[0] = d.raster.scissor[1] = 0;
  d.raster.scissor[2] = d.raster.scissor[3] = 4;
  renderer.draw(d);
  auto pixels = renderer.read(d.color);
  nearPixel(pixels, 1, 1, {255, 0, 0, 255}, "Scissor inside");
  nearPixel(pixels, 8, 8, {255, 128, 191, 255}, "Scissor outside");
  auto copy = image();
  renderer.copy(d.color, copy);
  nearPixel(renderer.read(copy), 8, 8, {255, 128, 191, 255},
            "Texture copy orientation");
  uint32_t rect[] = {8, 0, 8, 8};
  float green[] = {0, 1, 0, 1};
  renderer.clear(copy, {}, 1, green, 1, 0, rect);
  pixels = renderer.read(copy);
  nearPixel(pixels, 12, 4, {0, 255, 0, 255}, "Partial clear inside");
  nearPixel(pixels, 4, 4, {255, 128, 191, 255}, "Partial clear outside");
  // Alpha blend and programmable alpha test must share D3D semantics.
  d.color = copy;
  d.raster.scissorOn = false;
  d.raster.blendOn = true;
  d.raster.src = Blend::SrcAlpha;
  d.raster.dst = Blend::InvSrcAlpha;
  d.raster.srcAlpha = Blend::One;
  d.raster.dstAlpha = Blend::Zero;
  constants[0] = 0;
  constants[1] = 0;
  constants[2] = 1;
  constants[3] = 0.5;
  d.psConstants = packConstants(*d.ps, constants, ints, bools);
  renderer.draw(d);
  nearPixel(renderer.read(copy), 12, 4, {0, 128, 128, 128}, "Alpha blending");
  d.effects[0] = 1;
  d.effects[1] = 5;
  d.effects[2] = 0.75;
  constants[0] = 1;
  d.psConstants = packConstants(*d.ps, constants, ints, bools);
  renderer.draw(d);
  nearPixel(renderer.read(copy), 12, 4, {0, 128, 128, 128}, "Alpha rejection");
  auto view = std::make_shared<Image>();
  view->parent = copy;
  view->width = copy->width;
  view->height = copy->height;
  nearPixel(renderer.read(view), 12, 4, {0, 128, 128, 128},
            "Texture view readback");
  // Cross uniform-page boundaries and recycle pages with two frames in flight.
  // Every frame targets a separate image, so premature reuse changes its
  // pixels.
  Draw stress = d;
  stress.vs = testVS();
  stress.vsConstants.clear();
  stress.effects = {};
  stress.raster.depthOn = false;
  stress.raster.depthWrite = false;
  stress.raster.blendOn = false;
  stress.raster.scissorOn = true;
  stress.raster.colorMask = 15;
  stress.depth.reset();
  stress.indices = std::make_shared<Buffer>();
  uint16_t indices[] = {0, 1, 2};
  stress.indices->bytes.resize(sizeof(indices));
  std::memcpy(stress.indices->bytes.data(), indices, sizeof(indices));
  stress.indexSize = 2;
  std::array<std::shared_ptr<Image>, 4> targets;
  const std::array<int, 4> colors[] = {{255, 0, 0, 255},
                                       {0, 255, 0, 255},
                                       {0, 0, 255, 255},
                                       {255, 255, 255, 255}};
  for (unsigned frame = 0; frame < targets.size(); ++frame) {
    targets[frame] = image();
    stress.color = targets[frame];
    for (unsigned draw = 0; draw < 544; ++draw) {
      unsigned q = draw % 4;
      stress.raster.scissor[0] = (q % 2) * 8;
      stress.raster.scissor[1] = (q / 2) * 8;
      stress.raster.scissor[2] = stress.raster.scissor[3] = 8;
      float value[1024]{};
      for (unsigned c = 0; c < 4; ++c)
        value[c] = colors[(q + frame) % 4][c] / 255.0f;
      stress.psConstants = packConstants(*stress.ps, value, ints, bools);
      renderer.draw(stress);
    }
    renderer.present(targets[frame]);
  }
  renderer.wait();
  for (unsigned frame = 0; frame < targets.size(); ++frame) {
    auto pixels = renderer.read(targets[frame]);
    for (unsigned q = 0; q < 4; ++q)
      nearPixel(pixels, (q % 2) * 8 + 4, (q / 2) * 8 + 4,
                colors[(q + frame) % 4], "Uniform page rollover/reuse");
  }
  std::puts("[Metal check] indexed draw stress: 2176 draws, uniform page "
            "rollover/reuse and in-flight pixel isolation passed");
  {
    Draw scissor = stress;
    scissor.indices.reset();
    scissor.color = image();
    scissor.raster.scissorOn = true;
    scissor.raster.scissor[0] = scissor.raster.scissor[1] = 0;
    scissor.raster.scissor[2] = scissor.raster.scissor[3] = 4;
    float color[1024] = {1, 0, 0, 1};
    scissor.psConstants = packConstants(*scissor.ps, color, ints, bools);
    renderer.draw(scissor);
    scissor.raster.scissorOn = false;
    color[0] = 0;
    color[1] = 1;
    scissor.psConstants = packConstants(*scissor.ps, color, ints, bools);
    renderer.draw(scissor);
    auto pixels = renderer.read(scissor.color);
    nearPixel(pixels, 1, 1, {0, 255, 0, 255},
              "Reused pass scissor reset inside");
    nearPixel(pixels, 12, 12, {0, 255, 0, 255},
              "Reused pass scissor reset outside");
  }
  Draw nullCube = d;
  nullCube.color = image();
  nullCube.depth.reset();
  nullCube.vs = testVS();
  nullCube.vsConstants.clear();
  nullCube.ps = std::make_shared<Shader>();
  nullCube.ps->entry = "nullCubePS";
  nullCube.ps->samplerMask = nullCube.ps->cubeSamplerMask = 1;
  nullCube.ps->source = R"MSL(#include <metal_stdlib>
using namespace metal;
fragment float4 nullCubePS(texturecube<float> image [[texture(0)]],sampler smp [[sampler(0)]]){return image.sample(smp,float3(1,0,0));}
)MSL";
  nullCube.ps->hash =
      shaderHash(nullCube.ps->source.data(), nullCube.ps->source.size());
  nullCube.psConstants.clear();
  nullCube.raster.depthOn = nullCube.raster.depthWrite =
      nullCube.raster.blendOn = nullCube.raster.scissorOn = false;
  nullCube.raster.colorMask = 15;
  nullCube.effects = {};
  renderer.draw(nullCube);
  nearPixel(renderer.read(nullCube.color), 8, 8, {0, 0, 0, 255},
            "Null cube sampler");
  renderer.present(copy);
  renderer.wait();
  std::puts("[Metal check] GPU: triangle, constants, depth, color mask, "
            "scissor, copy, partial clear, blend, alpha test, views, readback "
            "and submit passed");
  return fo2CheckMetalAdapter();
}
} // namespace fo2render
