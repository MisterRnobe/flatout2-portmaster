#pragma once
// The Metal implementation consumes these owned types, never guest pointers or
// COM interfaces.
#include "debug_view.h"
#include "material.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace fo2render {
using Bytes = std::vector<uint8_t>;
struct Buffer {
  bool transient = false;
  Bytes bytes;
  uint64_t version = 1;
  std::shared_ptr<void> gpu;
};
struct Image {
  std::shared_ptr<Image> parent;
  uint32_t baseLevel = 0, baseFace = 0;
  uint32_t width = 1, height = 1, levels = 1, faces = 1;
  bool depth = false, stencil = false;
  uint64_t version = 1;
  std::vector<Bytes> rgba;
  std::shared_ptr<void> gpu;
};
enum class Element {
  Float1,
  Float2,
  Float3,
  Float4,
  Color,
  Byte4,
  Short2,
  Short4,
  Byte4N,
  Short2N,
  Short4N,
  UShort2N,
  UShort4N,
  Half2,
  Half4
};
struct Attribute {
  uint32_t slot = 0, stream = 0, offset = 0;
  Element type = Element::Float4;
};
struct Uniform {
  uint32_t kind = 0, index = 0, count = 1;
};
struct Shader {
  bool vertex = false;
  bool terrainSpecularMask = false;
  uint8_t physicalWaterFamily = 0;
  std::string source, entry;
  std::vector<Uniform> uniforms;
  std::vector<std::array<uint32_t, 3>> inputs;
  uint32_t floats = 0, ints = 0, bools = 0;
  uint32_t samplerMask = 0, cubeSamplerMask = 0;
  uint64_t hash = 0;
  std::shared_ptr<void> gpu;
  std::shared_ptr<Shader> materialVariant,waterVariant;
  uint32_t capturedMaterialCues=0;
};
// One Metal library may expose several functions with identical source hashes.
inline std::string pipelineShaderIdentity(const Shader& shader){
  return std::to_string(shader.hash)+":"+(shader.vertex?"v:":"f:")+
      std::to_string(shader.entry.size())+":"+shader.entry;
}
enum class Compare {
  Never,
  Less,
  Equal,
  LessEqual,
  Greater,
  NotEqual,
  GreaterEqual,
  Always
};
enum class Blend {
  Zero,
  One,
  SrcColor,
  InvSrcColor,
  SrcAlpha,
  InvSrcAlpha,
  DstAlpha,
  InvDstAlpha,
  DstColor,
  InvDstColor,
  SrcAlphaSat,
  Constant,
  InvConstant
};
enum class BlendOp { Add, Subtract, ReverseSubtract, Min, Max };
enum class StencilOp {
  Keep,
  Zero,
  Replace,
  IncrementClamp,
  DecrementClamp,
  Invert,
  IncrementWrap,
  DecrementWrap
};
enum class Primitive { Points, Lines, LineStrip, Triangles, TriangleStrip };
struct Sampler {
  uint32_t address[3] = {1, 1, 1};
  bool minLinear = true, magLinear = true, mipLinear = false,
       mipEnabled = false;
  float lodBias = 0, maxAnisotropy = 1, minLod = 0;
};
struct Raster {
  float viewport[6] = {0, 0, 1280, 800, 0, 1};
  uint32_t scissor[4] = {0, 0, 1280, 800};
  bool scissorOn = false, depthOn = true, depthWrite = true, blendOn = false;
  Compare depthCompare = Compare::LessEqual;
  uint32_t cull = 0, fill = 0, colorMask = 15;
  Blend src = Blend::One, dst = Blend::Zero, srcAlpha = Blend::One,
        dstAlpha = Blend::Zero;
  BlendOp blend = BlendOp::Add, blendAlpha = BlendOp::Add;
  float blendColor[4] = {1, 1, 1, 1};
  bool stencilOn = false;
  Compare stencilCompare = Compare::Always;
  StencilOp stencilFail = StencilOp::Keep, stencilDepthFail = StencilOp::Keep,
            stencilPass = StencilOp::Keep;
  // Front means clockwise, matching the renderer's fixed front-face winding.
  Compare backStencilCompare = Compare::Always;
  StencilOp backStencilFail = StencilOp::Keep,
            backStencilDepthFail = StencilOp::Keep,
            backStencilPass = StencilOp::Keep;
  bool twoSidedStencil = false;
  uint32_t stencilRead = 255, stencilWrite = 255, stencilRef = 0;
  float depthBias = 0, slopeBias = 0;
};
// Constant buffers must match the packed MSL uniforms of each shader.
struct Draw {
  std::shared_ptr<Image> color, depth, materialTarget;
  MaterialHint material;
  bool captureMaterial=false,waterBasisValid=false,hudBoundary=false;
  bool rayOnly=false;
  uint32_t physicalWaterAsset=0; // Semantic collision identity captured with this draw.
  std::array<float,9> waterBasis{};
  std::shared_ptr<Shader> vs, ps;
  Bytes vsConstants, psConstants;
  std::vector<Attribute> attributes;
  std::array<std::shared_ptr<Buffer>, 16> streams;
  std::array<uint32_t, 16> strides{}, offsets{}, stepRates{};
  uint32_t perInstanceMask = 0, instanceCount = 1, textureSrgbMask = 0;
  bool colorSrgb = false;
  std::array<std::shared_ptr<Image>, 16> textures;
  std::array<Sampler, 16> samplers;
  std::shared_ptr<Buffer> indices;
  uint32_t indexSize = 2, indexStart = 0, vertexStart = 0, count = 0;
  int32_t baseVertex = 0;
  Primitive primitive = Primitive::Triangles;
  Raster raster;
  std::array<float, 64> effects{};
  DebugProjection debugProjection;

};
class Renderer {
  struct Impl;
  std::unique_ptr<Impl> impl;

public:
  Renderer();
  ~Renderer();
  Renderer(const Renderer &) = delete;
  void attach(void *sdlWindow);
  void resize(uint32_t, uint32_t);
  void overlay(const std::shared_ptr<Image> &, const char *);
  void validateShader(const std::shared_ptr<Shader> &);
  void draw(const Draw &);
  void clear(const std::shared_ptr<Image> &, const std::shared_ptr<Image> &,
             uint32_t, const float *, float, uint32_t,
             const uint32_t *rect = nullptr);
  void copy(const std::shared_ptr<Image> &, const std::shared_ptr<Image> &,
            const uint32_t *sourceRect = nullptr,
            const uint32_t *targetRect = nullptr, bool linear = true);
  Bytes read(const std::shared_ptr<Image> &);
  void setDebugCapture(bool, bool color=false, bool materials=false);
  std::shared_ptr<Image> screenReflections(const std::shared_ptr<Image>&, bool mask=false, bool enabled=true);
  std::shared_ptr<Image> debugView(DebugView, float range=200);
  bool captureReflectionDiagnostics(const std::string& directory,const std::shared_ptr<Image>& base={});
  void present(const std::shared_ptr<Image> &);
  void wait();
  std::string deviceName() const;
  uint64_t frames() const;
};
std::shared_ptr<Shader> translateShader(const std::vector<uint8_t> &, bool,
                                        uint32_t colorMask = 0,
                                        uint32_t cubeMask = 0,
                                        bool vertexFog = false);
Bytes packConstants(const Shader &, const float *, const int32_t *,
                    const uint32_t *);
int checkMetalShaders(const char *directory, bool gpu);
int checkMetalRenderer();
int checkMetalWaterTrace();
int checkMetalRayTracing(bool gpu=true);
int checkMetalRayTracingScene(const char* directory,bool gpu=true);
int replayTraceMismatch(const char* directory);
} // namespace fo2render
