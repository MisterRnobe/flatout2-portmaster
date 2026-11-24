#include "../guest/dxt_decode.h"
#include "../render/backend.h"
#include "../render/commands.h"
#include "../render/material_shader.h"
#include "../render/water.h"
#include "../render/terrain_specular.h"
#include "../render/hud_boundary.h"
#include "../render/particle_strip.h"
#include "../render/row_upload.h"
#include "../render/command_payload.h"
#include "../render/reference_deltas.h"
#include "../render/shader_capture.h"
#include "d3d9_stubs.h"
#include "effects.h"
#include "fixed_functions.h"
#include "pixels.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <stdexcept>
extern int fo2CheckMetalBridgeResetCPU();
extern void fo2MetalPrepareWindow(void *);
extern void fo2MetalDestroyWindow();
extern bool fo2MetalRaceActive();
extern bool fo2MetalSSREnabled();
extern "C" uint32_t fo2WaterPhysicalAsset();
extern "C" int fo2OffcameraRayOnlyDraw();
static thread_local int metalRayOnly=0;
extern "C" int fo2MetalSetRayOnly(int value){int previous=metalRayOnly;metalRayOnly=value>=0&&value<=2?value:0;return previous;}
extern const char* fo2MetalWaterTraceLabel();
extern void fo2MetalSetDebugReady(bool);
namespace fo2metal {
using namespace fo2render;
static IDirect3DDevice9 *currentDevice = nullptr;
static void *hostWindow = nullptr;
static uint32_t renderWidth = 1280, renderHeight = 800;
static std::atomic<uint32_t> presentCount{0};
static std::atomic<bool> showFps{true}, screenshot{false};
template <class T> class Ref : public T {
  std::atomic<ULONG> refs{1};

public:
  virtual ~Ref() = default;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **out) override {
    if (!out)
      return E_POINTER;
    *out = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    auto n = --refs;
    if (!n)
      delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **out) {
    if (!out || !currentDevice)
      return D3DERR_INVALIDCALL;
    *out = currentDevice;
    currentDevice->AddRef();
    return D3D_OK;
  }
};
template <class T> std::shared_ptr<T> retain(T *p) {
  if (p)
    p->AddRef();
  return std::shared_ptr<T>(p, [](T *q) {
    if (q)
      q->Release();
  });
}
template <class T> std::shared_ptr<T> adopt(T *p) {
  return std::shared_ptr<T>(p, [](T *q) {
    if (q)
      q->Release();
  });
}
template <class T> HRESULT output(const std::shared_ptr<T> &value, T **out) {
  if (!out)
    return E_POINTER;
  *out = value.get();
  if (*out)
    (*out)->AddRef();
  return D3D_OK;
}
static float asFloat(DWORD value) {
  float f;
  std::memcpy(&f, &value, 4);
  return f;
}
static bool compressed(D3DFORMAT f) {
  return f >= D3DFMT_DXT1 && f <= D3DFMT_DXT5;
}
static std::shared_ptr<Image> imageView(const std::shared_ptr<Image> &,
                                        unsigned, unsigned);
struct TextureData {
  std::shared_ptr<Image> image = std::make_shared<Image>();
  std::shared_ptr<Renderer> renderer;
  D3DFORMAT format;
  DWORD usage;
  D3DPOOL pool;
  std::vector<Bytes> raw;
  // Surface cache lookup/AddRef and final Release/erasure form one operation.
  // The game thread may query while the render thread unbinds a surface.
  std::recursive_mutex surfaceMutex;
  TextureData(uint32_t w, uint32_t h, uint32_t levels, uint32_t faces,
              D3DFORMAT f, DWORD u, D3DPOOL p, std::shared_ptr<Renderer> r)
      : renderer(r), format(f), usage(u), pool(p) {
    if (!w || !h || w > 16384 || h > 16384)
      throw std::runtime_error("Invalid texture dimensions");
    if (!levels) {
      levels = 1;
      for (uint32_t x = std::max(w, h); x > 1; x >>= 1)
        ++levels;
    }
    image->width = w;
    image->height = h;
    image->levels = levels;
    image->faces = faces;
    image->depth = (u & D3DUSAGE_DEPTHSTENCIL) != 0;
    image->stencil = f == D3DFMT_D24S8 || f == D3DFMT_D24FS8;
    raw.resize(levels * faces);
    image->rgba.resize(levels * faces);
    for (uint32_t face = 0; face < faces; ++face)
      for (uint32_t level = 0; level < levels; ++level) {
        raw[face * levels + level].resize(pitch(level) * rows(level));
        if (!image->depth)
          image->rgba[face * levels + level].resize(size_t(width(level)) *
                                                    height(level) * 4);
      }
  }
  uint32_t width(uint32_t l) const { return std::max(1u, image->width >> l); }
  uint32_t height(uint32_t l) const { return std::max(1u, image->height >> l); }
  uint32_t pitch(uint32_t l) const {
    return compressed(format)
               ? ((width(l) + 3) / 4) * (format == D3DFMT_DXT1 ? 8 : 16)
               : width(l) * d3dFormatBytesPerPixel(format);
  }
  uint32_t rows(uint32_t l) const {
    return compressed(format) ? (height(l) + 3) / 4 : height(l);
  }
  void upload(uint32_t face, uint32_t level) {
    if (image->depth)
      return;
    auto &source = raw.at(face * image->levels + level);
    auto &dest = image->rgba.at(face * image->levels + level);
    if (compressed(format))
      dest =
          decodeDXT(format == D3DFMT_DXT1                            ? 1
                    : format == D3DFMT_DXT2 || format == D3DFMT_DXT3 ? 3
                                                                     : 5,
                    width(level), height(level), source.data(), source.size());
    else
      for (uint32_t y = 0; y < height(level); ++y)
        convertRowToRGBA8(format, source.data() + size_t(y) * pitch(level),
                          dest.data() + size_t(y) * width(level) * 4,
                          width(level));
    ++image->version;
  }
  void storeRGBA(uint32_t face, uint32_t level, const Bytes &rgba) {
    auto &dest = raw.at(face * image->levels + level);
    if (format != D3DFMT_A8R8G8B8 && format != D3DFMT_X8R8G8B8 &&
        format != D3DFMT_A8B8G8R8)
      throw std::runtime_error("Unsupported render target readback format");
    if (rgba.size() != size_t(width(level)) * height(level) * 4)
      throw std::runtime_error("Readback size mismatch");
    for (size_t i = 0; i < rgba.size(); i += 4) {
      dest[i] = format == D3DFMT_A8B8G8R8 ? rgba[i] : rgba[i + 2];
      dest[i + 1] = rgba[i + 1];
      dest[i + 2] = format == D3DFMT_A8B8G8R8 ? rgba[i + 2] : rgba[i];
      dest[i + 3] = format == D3DFMT_X8R8G8B8 ? 255 : rgba[i + 3];
    }
    image->rgba[face * image->levels + level] = rgba;
  }
  void download(uint32_t face, uint32_t level) {
    if (usage & D3DUSAGE_RENDERTARGET)
      storeRGBA(face, level, renderer->read(imageView(image, face, level)));
  }
  HRESULT desc(uint32_t level, D3DSURFACE_DESC *d) {
    if (!d || level >= image->levels)
      return D3DERR_INVALIDCALL;
    *d = {};
    d->Format = format;
    d->Type = D3DRTYPE_SURFACE;
    d->Usage = usage;
    d->Pool = pool;
    d->MultiSampleType = D3DMULTISAMPLE_NONE;
    d->Width = width(level);
    d->Height = height(level);
    return D3D_OK;
  }
};
static std::shared_ptr<Image> imageView(const std::shared_ptr<Image> &image,
                                        unsigned face, unsigned level) {
  if (!face && !level && image->faces == 1)
    return image;
  auto view = std::make_shared<Image>();
  view->parent = image;
  view->baseLevel = level;
  view->baseFace = face;
  view->width = std::max(1u, image->width >> level);
  view->height = std::max(1u, image->height >> level);
  view->depth = image->depth;
  view->stencil = image->stencil;
  return view;
}
class Surface : public Ref<IDirect3DSurface9Stub> {
public:
  std::shared_ptr<TextureData> data;
  std::shared_ptr<Image> view;
  uint32_t face, level;
  bool writeLock = false, locked = false;
  IUnknown *container = nullptr;
  std::function<void()> onDestroy;
  Surface(std::shared_ptr<TextureData> d, uint32_t f = 0, uint32_t l = 0,
          IUnknown *c = nullptr)
      : data(d), face(f), level(l), container(c) {
    view = imageView(data->image, f, l);
    if (c)
      c->AddRef();
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    std::lock_guard<std::recursive_mutex> lock(data->surfaceMutex);
    return Ref<IDirect3DSurface9Stub>::AddRef();
  }
  ULONG STDMETHODCALLTYPE Release() override {
    // Keep the mutex alive even when this Release destroys the last surface
    // and its parent texture. Never read members after the base Release.
    auto owner = data;
    std::lock_guard<std::recursive_mutex> lock(owner->surfaceMutex);
    return Ref<IDirect3DSurface9Stub>::Release();
  }
  ~Surface() {
    if (onDestroy)
      onDestroy();
    if (container)
      container->Release();
  }
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override {
    return D3DRTYPE_SURFACE;
  }
  HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC *d) override {
    return data->desc(level, d);
  }
  HRESULT STDMETHODCALLTYPE GetContainer(REFIID iid, void **out) override {
    return container ? container->QueryInterface(iid, out) : E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT *out, const RECT *rect,
                                     DWORD flags) override {
    if (!out || locked)
      return D3DERR_INVALIDCALL;
    data->download(face, level);
    int x = rect ? rect->left : 0, y = rect ? rect->top : 0;
    if (x < 0 || y < 0 || uint32_t(x) >= data->width(level) ||
        uint32_t(y) >= data->height(level))
      return D3DERR_INVALIDCALL;
    if (rect && (rect->right > LONG(data->width(level)) ||
                 rect->bottom > LONG(data->height(level)) || rect->right <= x ||
                 rect->bottom <= y))
      return D3DERR_INVALIDCALL;
    locked = true;
    writeLock = !(flags & D3DLOCK_READONLY);
    out->Pitch = data->pitch(level);
    size_t offset =
        compressed(data->format)
            ? size_t(y / 4) * out->Pitch +
                  (x / 4) * (data->format == D3DFMT_DXT1 ? 8 : 16)
            : size_t(y) * out->Pitch + x * d3dFormatBytesPerPixel(data->format);
    out->pBits =
        data->raw.at(face * data->image->levels + level).data() + offset;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE UnlockRect() override {
    if (!locked)
      return D3DERR_INVALIDCALL;
    locked = false;
    if (writeLock)
      data->upload(face, level);
    writeLock = false;
    return D3D_OK;
  }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
  void STDMETHODCALLTYPE PreLoad() override {}
};
template <class Stub> class Texture : public Ref<Stub> {
  std::map<uint32_t, Surface *> surfaces;

public:
  std::shared_ptr<TextureData> data;
  DWORD lod = 0;
  D3DTEXTUREFILTERTYPE filter = D3DTEXF_LINEAR;
  explicit Texture(std::shared_ptr<TextureData> d) : data(d) {}
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
  void STDMETHODCALLTYPE PreLoad() override {}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override {
    return data->image->faces == 6 ? D3DRTYPE_CUBETEXTURE : D3DRTYPE_TEXTURE;
  }
  DWORD STDMETHODCALLTYPE SetLOD(DWORD value) override {
    auto old = lod;
    lod = value;
    return old;
  }
  DWORD STDMETHODCALLTYPE GetLOD() override { return lod; }
  DWORD STDMETHODCALLTYPE GetLevelCount() override {
    return data->image->levels;
  }
  HRESULT STDMETHODCALLTYPE
  SetAutoGenFilterType(D3DTEXTUREFILTERTYPE f) override {
    filter = f;
    return D3D_OK;
  }
  D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override {
    return filter;
  }
  void STDMETHODCALLTYPE GenerateMipSubLevels() override;
  HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level,
                                         D3DSURFACE_DESC *d) override {
    return data->desc(level, d);
  }
  HRESULT surface(uint32_t face, uint32_t level, IDirect3DSurface9 **out) {
    if (!out || face >= data->image->faces || level >= data->image->levels)
      return D3DERR_INVALIDCALL;
    std::lock_guard<std::recursive_mutex> lock(data->surfaceMutex);
    uint32_t id = face * data->image->levels + level;
    auto i = surfaces.find(id);
    if (i != surfaces.end()) {
      *out = i->second;
      (*out)->AddRef();
      return D3D_OK;
    }
    auto *s = new Surface(data, face, level, this);
    s->onDestroy = [this, id] { surfaces.erase(id); };
    surfaces[id] = s;
    *out = s;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT l, IDirect3DSurface9 **out) {
    return surface(0, l, out);
  }
  HRESULT STDMETHODCALLTYPE GetCubeMapSurface(D3DCUBEMAP_FACES f, UINT l,
                                              IDirect3DSurface9 **out) {
    return surface(f, l, out);
  }
  std::map<uint32_t, std::shared_ptr<Surface>> locks;
  HRESULT lock(uint32_t face, uint32_t level, D3DLOCKED_RECT *out,
               const RECT *rect, DWORD flags) {
    if (face >= data->image->faces || level >= data->image->levels)
      return D3DERR_INVALIDCALL;
    uint32_t id = face * data->image->levels + level;
    if (locks.count(id))
      return D3DERR_INVALIDCALL;
    auto s = adopt(new Surface(data, face, level));
    auto hr = s->LockRect(out, rect, flags);
    if (SUCCEEDED(hr))
      locks[id] = s;
    return hr;
  }
  HRESULT unlock(uint32_t face, uint32_t level) {
    uint32_t id = face * data->image->levels + level;
    auto i = locks.find(id);
    if (i == locks.end())
      return D3DERR_INVALIDCALL;
    auto hr = i->second->UnlockRect();
    locks.erase(i);
    return hr;
  }
  HRESULT STDMETHODCALLTYPE LockRect(UINT l, D3DLOCKED_RECT *out,
                                     const RECT *rect, DWORD flags) {
    return lock(0, l, out, rect, flags);
  }
  HRESULT STDMETHODCALLTYPE UnlockRect(UINT l) { return unlock(0, l); }
  HRESULT STDMETHODCALLTYPE LockRect(D3DCUBEMAP_FACES f, UINT l,
                                     D3DLOCKED_RECT *out, const RECT *rect,
                                     DWORD flags) {
    return lock(f, l, out, rect, flags);
  }
  HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES f, UINT l) {
    return unlock(f, l);
  }
  HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT *) { return D3D_OK; }
  HRESULT STDMETHODCALLTYPE AddDirtyRect(D3DCUBEMAP_FACES, const RECT *) {
    return D3D_OK;
  }
};
using Texture2D = Texture<IDirect3DTexture9Stub>;
using TextureCube = Texture<IDirect3DCubeTexture9Stub>;
template <class Stub> void Texture<Stub>::GenerateMipSubLevels() {
  for (unsigned f = 0; f < data->image->faces; ++f)
    data->download(f, 0);
  for (uint32_t face = 0; face < data->image->faces; ++face)
    for (uint32_t l = 1; l < data->image->levels; ++l) {
      auto &prev = data->image->rgba[face * data->image->levels + l - 1];
      auto &dst = data->image->rgba[face * data->image->levels + l];
      uint32_t w = data->width(l), h = data->height(l), pw = data->width(l - 1),
               ph = data->height(l - 1);
      for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
          for (unsigned c = 0; c < 4; ++c) {
            unsigned sum = 0;
            for (unsigned dy = 0; dy < 2; ++dy)
              for (unsigned dx = 0; dx < 2; ++dx)
                sum += prev[(size_t(std::min(ph - 1, y * 2 + dy)) * pw +
                             std::min(pw - 1, x * 2 + dx)) *
                                4 +
                            c];
            dst[(size_t(y) * w + x) * 4 + c] = sum / 4;
          }
    }
  ++data->image->version;
}
template <class Stub> class BufferObject : public Ref<Stub> {
public:
  std::shared_ptr<Buffer> data = std::make_shared<Buffer>();
  DWORD usage, fvf;
  D3DFORMAT format;
  D3DPOOL pool;
  bool writeLock = false;
  DWORD lastLockFlags = 0;
  uint64_t discards = 0;
  BufferObject(UINT size, DWORD u, DWORD f, D3DFORMAT fmt, D3DPOOL p)
      : usage(u), fvf(f), format(fmt), pool(p) {
    data->bytes.resize(size);
  }
  HRESULT STDMETHODCALLTYPE Lock(UINT offset, UINT size, void **out,
                                 DWORD flags) override {
    if (!out || offset > data->bytes.size() ||
        (size && size > data->bytes.size() - offset))
      return D3DERR_INVALIDCALL;
    lastLockFlags = flags;
    if (flags & D3DLOCK_DISCARD) {
      ++discards;
      // Match the guest mirror: a partial DISCARD still invalidates the
      // entire buffer. Unwritten particle slots must not retain old vertices.
      std::fill(data->bytes.begin(), data->bytes.end(), 0);
    }
    *out = data->bytes.data() + offset;
    writeLock = !(flags & D3DLOCK_READONLY);
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE Unlock() override {
    if (writeLock)
      ++data->version;
    writeLock = false;
    return D3D_OK;
  }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
  void STDMETHODCALLTYPE PreLoad() override {}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override {
    return format == D3DFMT_VERTEXDATA ? D3DRTYPE_VERTEXBUFFER
                                       : D3DRTYPE_INDEXBUFFER;
  }
  HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC *out) {
    if (!out)
      return E_POINTER;
    *out = {};
    out->Format = format;
    out->Type = D3DRTYPE_VERTEXBUFFER;
    out->Usage = usage;
    out->Pool = pool;
    out->Size = data->bytes.size();
    out->FVF = fvf;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC *out) {
    if (!out)
      return E_POINTER;
    *out = {};
    out->Format = format;
    out->Type = D3DRTYPE_INDEXBUFFER;
    out->Usage = usage;
    out->Pool = pool;
    out->Size = data->bytes.size();
    return D3D_OK;
  }
};
using VertexBuffer = BufferObject<IDirect3DVertexBuffer9Stub>;
using IndexBuffer = BufferObject<IDirect3DIndexBuffer9Stub>;
static Bytes shaderBytes(const DWORD *tokens) {
  if (!tokens)
    throw std::runtime_error("Null shader bytecode");
  size_t words = 1;
  while (words < 65536) {
    DWORD op = tokens[words];
    if (op == 0x0000ffff) {
      ++words;
      return Bytes(reinterpret_cast<const uint8_t *>(tokens),
                   reinterpret_cast<const uint8_t *>(tokens + words));
    }
    if ((op & 0xffff) == 0xfffe)
      words += 1 + ((op >> 16) & 0x7fff);
    else if (((tokens[0] >> 8) & 255) >= 2)
      words += 1 + ((op >> 24) & 15);
    else
      ++words;
  }
  throw std::runtime_error("Unterminated shader bytecode");
}
template <class Stub> class ShaderObject : public Ref<Stub> {
public:
  Bytes bytecode;
  bool vertex;
  uint64_t codeHash;
  std::map<uint64_t, std::shared_ptr<Shader>> variants;
  ShaderObject(const DWORD *tokens, bool v)
      : bytecode(shaderBytes(tokens)), vertex(v),codeHash(shaderHash(bytecode.data(),bytecode.size())) {
    captureShader(bytecode.data(), bytecode.size());
  }
  HRESULT STDMETHODCALLTYPE GetFunction(void *out, UINT *size) override {
    if (!size)
      return E_POINTER;
    if (!out) {
      *size = bytecode.size();
      return D3D_OK;
    }
    if (*size < bytecode.size())
      return D3DERR_INVALIDCALL;
    std::memcpy(out, bytecode.data(), bytecode.size());
    *size = bytecode.size();
    return D3D_OK;
  }
  std::shared_ptr<Shader> translated(uint32_t colors, uint32_t cubes,
                                     bool fog = false) {
    if (vertex)
      cubes = 0;
    else
      colors = 0;
    uint64_t key = colors | (uint64_t(cubes) << 32) | (uint64_t(fog) << 48);
    auto &result = variants[key];
    if (!result)
      result = translateShader(bytecode, vertex, colors, cubes, fog);
    return result;
  }
};
using VertexShader = ShaderObject<IDirect3DVertexShader9Stub>;
using PixelShader = ShaderObject<IDirect3DPixelShader9Stub>;
class Declaration : public Ref<IDirect3DVertexDeclaration9Stub> {
public:
  std::vector<D3DVERTEXELEMENT9> elements;
  explicit Declaration(const D3DVERTEXELEMENT9 *p) {
    if (!p)
      throw std::runtime_error("Null declaration");
    for (unsigned i = 0; i < 65; ++i) {
      elements.push_back(p[i]);
      if (p[i].Stream == 0xff)
        return;
    }
    throw std::runtime_error("Unterminated declaration");
  }
  HRESULT STDMETHODCALLTYPE GetDeclaration(D3DVERTEXELEMENT9 *out,
                                           UINT *count) override {
    if (!count)
      return E_POINTER;
    if (out && *count < elements.size())
      return D3DERR_INVALIDCALL;
    if (out)
      std::memcpy(out, elements.data(), elements.size() * sizeof(*out));
    *count = elements.size();
    return D3D_OK;
  }
};
struct State {
  std::array<DWORD, 256> rs{};
  std::array<std::array<DWORD, 16>, 16> ss{};
  std::array<std::array<DWORD, 33>, 8> tss{};
  std::array<float, 1024> vsF{}, psF{};
  std::array<int32_t, 64> vsI{}, psI{};
  std::array<uint32_t, 16> vsB{}, psB{};
  std::array<std::shared_ptr<IDirect3DBaseTexture9>, 16> textures;
  std::array<std::shared_ptr<IDirect3DVertexBuffer9>, 16> streams;
  std::array<UINT, 16> offsets{}, strides{}, frequencies{};
  std::shared_ptr<IDirect3DIndexBuffer9> indices;
  std::shared_ptr<IDirect3DVertexShader9> vs;
  std::shared_ptr<IDirect3DPixelShader9> ps;
  std::shared_ptr<IDirect3DVertexDeclaration9> declaration;
  std::shared_ptr<IDirect3DSurface9> target, depth;
  DWORD fvf = 0;
  D3DVIEWPORT9 viewport{};
  RECT scissor{};
  std::map<D3DTRANSFORMSTATETYPE, D3DMATRIX> transforms;
  D3DMATERIAL9 material{};
};
struct Mask {
  std::bitset<256> rs, vf, pf;
  std::array<std::bitset<16>, 16> ss;
  std::array<std::bitset<33>, 8> tss;
  std::bitset<16> textures, streams, vi, pi, vb, pb;
  bool vs = false, ps = false, decl = false, fvf = false, viewport = false,
       scissor = false, indices = false, material = false;
  std::set<D3DTRANSFORMSTATETYPE> transforms;
};
static std::shared_ptr<Image> surfaceImage(IDirect3DSurface9 *);
class Device;
class StateBlock : public Ref<IDirect3DStateBlock9Stub> {
public:
  Device *device;
  State state;
  Mask mask;
  StateBlock(Device *, State, Mask);
  ~StateBlock();
  void trim();
  HRESULT STDMETHODCALLTYPE Capture() override;
  HRESULT STDMETHODCALLTYPE Apply() override;
};
class Adapter;
class Device : public Ref<IDirect3DDevice9Stub> {
public:
  std::shared_ptr<Renderer> renderer;
  Adapter *adapter;
  D3DPRESENT_PARAMETERS parameters{};
  State state, recordState;
  Mask recordMask;
  bool recording = false;
  std::shared_ptr<IDirect3DSurface9> back, initialDepth;
  Device(Adapter *, D3DPRESENT_PARAMETERS);
  ~Device();
  State &target() { return recording ? recordState : state; }
  HRESULT reset(D3DPRESENT_PARAMETERS *p);
  Draw prepare();
  HRESULT draw(D3DPRIMITIVETYPE, UINT, UINT, INT, bool);
  void apply(const State &, const Mask &);
  Mask allMask(D3DSTATEBLOCKTYPE);
  HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE,
                                        IDirect3DQuery9 **) override;
  HRESULT STDMETHODCALLTYPE GetSwapChain(UINT, IDirect3DSwapChain9 **) override;
  HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT,
                                            D3DRASTER_STATUS *p) override {
    if (!p)
      return E_POINTER;
    *p = {};
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9 *, const RECT *,
                                      D3DCOLOR) override;
  HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override { return D3D_OK; }
  UINT STDMETHODCALLTYPE GetAvailableTextureMem() override {
    return 512u << 20;
  }
  HRESULT STDMETHODCALLTYPE EvictManagedResources() override { return D3D_OK; }
  HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9 **) override;
  HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9 *) override;
  HRESULT STDMETHODCALLTYPE
  GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *p) override {
    if (!p)
      return E_POINTER;
    *p = {};
    p->DeviceType = D3DDEVTYPE_HAL;
    p->hFocusWindow = reinterpret_cast<HWND>(hostWindow);
    p->BehaviorFlags = D3DCREATE_HARDWARE_VERTEXPROCESSING;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT, D3DDISPLAYMODE *p) override {
    if (!p)
      return E_POINTER;
    *p = {parameters.BackBufferWidth, parameters.BackBufferHeight, 60,
          D3DFMT_X8R8G8B8};
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS *p) override {
    return reset(p);
  }
  HRESULT STDMETHODCALLTYPE Present(const RECT *, const RECT *, HWND,
                                    const RGNDATA *) override;
  HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT, UINT, D3DBACKBUFFER_TYPE,
                                          IDirect3DSurface9 **p) override {
    return output(back, p);
  }
  UINT STDMETHODCALLTYPE GetNumberOfSwapChains() override { return 1; }
  HRESULT STDMETHODCALLTYPE BeginScene() override { return D3D_OK; }
  HRESULT STDMETHODCALLTYPE EndScene() override { return D3D_OK; }
  HRESULT STDMETHODCALLTYPE Clear(DWORD, const D3DRECT *, DWORD, D3DCOLOR,
                                  float, DWORD) override;
  HRESULT STDMETHODCALLTYPE CreateTexture(UINT, UINT, UINT, DWORD, D3DFORMAT,
                                          D3DPOOL, IDirect3DTexture9 **,
                                          HANDLE *) override;
  HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT, UINT, DWORD, D3DFORMAT,
                                              D3DPOOL, IDirect3DCubeTexture9 **,
                                              HANDLE *) override;
  HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT, DWORD, DWORD, D3DPOOL,
                                               IDirect3DVertexBuffer9 **,
                                               HANDLE *) override;
  HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT, DWORD, D3DFORMAT, D3DPOOL,
                                              IDirect3DIndexBuffer9 **,
                                              HANDLE *) override;
  HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT, UINT, D3DFORMAT,
                                               D3DMULTISAMPLE_TYPE, DWORD,
                                               WINBOOL, IDirect3DSurface9 **,
                                               HANDLE *) override;
  HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT, UINT, D3DFORMAT,
                                                      D3DMULTISAMPLE_TYPE,
                                                      DWORD, WINBOOL,
                                                      IDirect3DSurface9 **,
                                                      HANDLE *) override;
  HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT, UINT, D3DFORMAT,
                                                        D3DPOOL,
                                                        IDirect3DSurface9 **,
                                                        HANDLE *) override;
  HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9 *, const RECT *,
                                          IDirect3DSurface9 *,
                                          const POINT *) override;
  HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9 *,
                                          IDirect3DBaseTexture9 *) override;
  HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9 *,
                                                IDirect3DSurface9 *) override;
  HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9 *, const RECT *,
                                        IDirect3DSurface9 *, const RECT *,
                                        D3DTEXTUREFILTERTYPE) override;
  HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD i,
                                            IDirect3DSurface9 *p) override {
    if (i)
      return D3DERR_INVALIDCALL;
    state.target = retain(p);
    if (auto s = dynamic_cast<Surface *>(p))
      state.viewport = {
          0, 0, s->data->width(s->level), s->data->height(s->level), 0, 1};
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD i,
                                            IDirect3DSurface9 **p) override {
    return i ? D3DERR_INVALIDCALL : output(state.target, p);
  }
  HRESULT STDMETHODCALLTYPE
  SetDepthStencilSurface(IDirect3DSurface9 *p) override {
    state.depth = retain(p);
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetDepthStencilSurface(IDirect3DSurface9 **p) override {
    return output(state.depth, p);
  }
  HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE t,
                                           DWORD v) override {
    if (t >= 256)
      return D3DERR_INVALIDCALL;
    target().rs[t] = v;
    if (recording)
      recordMask.rs[t] = true;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE t,
                                           DWORD *p) override {
    if (!p || t >= 256)
      return D3DERR_INVALIDCALL;
    *p = state.rs[t];
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD s, D3DSAMPLERSTATETYPE t,
                                            DWORD v) override {
    if (s >= 16 || t >= 16)
      return D3DERR_INVALIDCALL;
    target().ss[s][t] = v;
    if (recording)
      recordMask.ss[s][t] = true;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD s, D3DSAMPLERSTATETYPE t,
                                            DWORD *p) override {
    if (!p || s >= 16 || t >= 16)
      return D3DERR_INVALIDCALL;
    *p = state.ss[s][t];
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD s,
                                                 D3DTEXTURESTAGESTATETYPE t,
                                                 DWORD v) override {
    if (s >= 8 || t >= 33)
      return D3DERR_INVALIDCALL;
    target().tss[s][t] = v;
    if (recording)
      recordMask.tss[s][t] = true;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD s,
                                                 D3DTEXTURESTAGESTATETYPE t,
                                                 DWORD *p) override {
    if (!p || s >= 8 || t >= 33)
      return D3DERR_INVALIDCALL;
    *p = state.tss[s][t];
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetTexture(DWORD s,
                                       IDirect3DBaseTexture9 *p) override {
    if (s >= 16)
      return D3DERR_INVALIDCALL;
    target().textures[s] = retain(p);
    if (recording)
      recordMask.textures[s] = true;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTexture(DWORD s,
                                       IDirect3DBaseTexture9 **p) override {
    return s >= 16 ? D3DERR_INVALIDCALL : output(state.textures[s], p);
  }
  HRESULT STDMETHODCALLTYPE
  SetVertexShader(IDirect3DVertexShader9 *p) override {
    target().vs = retain(p);
    if (recording)
      recordMask.vs = true;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetVertexShader(IDirect3DVertexShader9 **p) override {
    return output(state.vs, p);
  }
  HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9 *p) override {
    target().ps = retain(p);
    if (recording)
      recordMask.ps = true;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9 **p) override {
    return output(state.ps, p);
  }
  HRESULT STDMETHODCALLTYPE
  SetVertexDeclaration(IDirect3DVertexDeclaration9 *p) override {
    target().declaration = retain(p);
    target().fvf = 0;
    if (recording) {
      recordMask.decl = true;
      recordMask.fvf = true;
    }
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  GetVertexDeclaration(IDirect3DVertexDeclaration9 **p) override {
    return output(state.declaration, p);
  }
  HRESULT STDMETHODCALLTYPE SetFVF(DWORD f) override {
    target().fvf = f;
    target().declaration.reset();
    if (recording) {
      recordMask.decl = true;
      recordMask.fvf = true;
    }
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetFVF(DWORD *p) override {
    if (!p)
      return E_POINTER;
    *p = state.fvf;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9 *p) override {
    target().indices = retain(p);
    if (recording)
      recordMask.indices = true;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9 **p) override {
    return output(state.indices, p);
  }
  HRESULT STDMETHODCALLTYPE SetStreamSource(UINT n, IDirect3DVertexBuffer9 *p,
                                            UINT offset, UINT stride) override {
    if (n >= 16)
      return D3DERR_INVALIDCALL;
    target().streams[n] = retain(p);
    target().offsets[n] = offset;
    target().strides[n] = stride;
    if (recording)
      recordMask.streams[n] = true;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetStreamSource(UINT n, IDirect3DVertexBuffer9 **p,
                                            UINT *offset,
                                            UINT *stride) override {
    if (n >= 16 || !offset || !stride)
      return D3DERR_INVALIDCALL;
    *offset = state.offsets[n];
    *stride = state.strides[n];
    return output(state.streams[n], p);
  }
  HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT n, UINT f) override {
    if (n >= 16)
      return D3DERR_INVALIDCALL;
    target().frequencies[n] = f;
    if (recording)
      recordMask.streams[n] = true;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT n, UINT *f) override {
    if (n >= 16 || !f)
      return D3DERR_INVALIDCALL;
    *f = state.frequencies[n];
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9 *p) override {
    if (!p)
      return E_POINTER;
    target().viewport = *p;
    if (recording)
      recordMask.viewport = true;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9 *p) override {
    if (!p)
      return E_POINTER;
    *p = state.viewport;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT *p) override {
    if (!p)
      return E_POINTER;
    target().scissor = *p;
    if (recording)
      recordMask.scissor = true;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetScissorRect(RECT *p) override {
    if (!p)
      return E_POINTER;
    *p = state.scissor;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE t,
                                         const D3DMATRIX *p) override {
    if (!p)
      return E_POINTER;
    target().transforms[t] = *p;
    if (recording)
      recordMask.transforms.insert(t);
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE t,
                                         D3DMATRIX *p) override {
    if (!p)
      return E_POINTER;
    *p = state.transforms[t];
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE,
                                              const D3DMATRIX *) override;
  HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9 *p) override {
    if (!p)
      return E_POINTER;
    target().material = *p;
    if (recording)
      recordMask.material = true;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9 *p) override {
    if (!p)
      return E_POINTER;
    *p = state.material;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  CreateVertexShader(const DWORD *p, IDirect3DVertexShader9 **out) override {
    if (!out)
      return E_POINTER;
    *out = new VertexShader(p, true);
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE
  CreatePixelShader(const DWORD *p, IDirect3DPixelShader9 **out) override {
    if (!out)
      return E_POINTER;
    *out = new PixelShader(p, false);
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(
      const D3DVERTEXELEMENT9 *p, IDirect3DVertexDeclaration9 **out) override {
    if (!out)
      return E_POINTER;
    *out = new Declaration(p);
    return D3D_OK;
  }
#define CONSTANTS(Stage, Letter, Type, Field, Count, Width, MaskField)         \
  HRESULT STDMETHODCALLTYPE Set##Stage##ShaderConstant##Letter(                \
      UINT first, const Type *p, UINT count) override {                        \
    if (!p || first > Count || count > Count - first)                          \
      return D3DERR_INVALIDCALL;                                               \
    std::memcpy(target().Field.data() + first * Width, p,                      \
                count * Width * sizeof(Type));                                 \
    if (recording)                                                             \
      for (UINT i = first; i < first + count; ++i)                             \
        recordMask.MaskField[i] = true;                                        \
    return D3D_OK;                                                             \
  }                                                                            \
  HRESULT STDMETHODCALLTYPE Get##Stage##ShaderConstant##Letter(                \
      UINT first, Type *p, UINT count) override {                              \
    if (!p || first > Count || count > Count - first)                          \
      return D3DERR_INVALIDCALL;                                               \
    std::memcpy(p, state.Field.data() + first * Width,                         \
                count * Width * sizeof(Type));                                 \
    return D3D_OK;                                                             \
  }
  CONSTANTS(Vertex, F, float, vsF, 256, 4, vf)
  CONSTANTS(Pixel, F, float, psF, 256, 4, pf)
  CONSTANTS(Vertex, I, int, vsI, 16, 4, vi)
  CONSTANTS(Pixel, I, int, psI, 16, 4, pi)
  CONSTANTS(Vertex, B, WINBOOL, vsB, 16, 1, vb)
  CONSTANTS(Pixel, B, WINBOOL, psB, 16, 1, pb)
#undef CONSTANTS
  HRESULT STDMETHODCALLTYPE BeginStateBlock() override {
    if (recording)
      return D3DERR_INVALIDCALL;
    recording = true;
    recordState = state;
    recordMask = {};
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9 **out) override;
  HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE,
                                             IDirect3DStateBlock9 **) override;
  HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE t, UINT start,
                                          UINT count) override {
    return draw(t, start, count, 0, false);
  }
  HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE t, INT base,
                                                 UINT, UINT, UINT start,
                                                 UINT count) override {
    return draw(t, start, count, base, true);
  }
  HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE, UINT,
                                            const void *, UINT) override;
  HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE, UINT, UINT,
                                                   UINT, const void *,
                                                   D3DFORMAT, const void *,
                                                   UINT) override;
  WINBOOL STDMETHODCALLTYPE SetSoftwareVertexProcessing(WINBOOL) override {
    return FALSE;
  }
  WINBOOL STDMETHODCALLTYPE GetSoftwareVertexProcessing() override {
    return FALSE;
  }
  HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD *p) override {
    if (p)
      *p = 1;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetDialogBoxMode(WINBOOL) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT, UINT,
                                                IDirect3DSurface9 *) override {
    return D3D_OK;
  }
  void STDMETHODCALLTYPE SetCursorPosition(int, int, DWORD) override {}
  WINBOOL STDMETHODCALLTYPE ShowCursor(WINBOOL v) override { return v; }
  void STDMETHODCALLTYPE SetGammaRamp(UINT, DWORD,
                                      const D3DGAMMARAMP *) override {}
  void STDMETHODCALLTYPE GetGammaRamp(UINT, D3DGAMMARAMP *p) override {
    if (p)
      for (int i = 0; i < 256; ++i)
        p->red[i] = p->green[i] = p->blue[i] = i * 257;
  }
};
class Query : public Ref<IDirect3DQuery9Stub> {
public:
  std::shared_ptr<Renderer> renderer;
  explicit Query(std::shared_ptr<Renderer> r) : renderer(r) {}
  D3DQUERYTYPE STDMETHODCALLTYPE GetType() override {
    return D3DQUERYTYPE_EVENT;
  }
  DWORD STDMETHODCALLTYPE GetDataSize() override { return sizeof(WINBOOL); }
  HRESULT STDMETHODCALLTYPE Issue(DWORD flags) override {
    if (flags != D3DISSUE_END)
      return D3DERR_INVALIDCALL;
    renderer->wait();
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetData(void *p, DWORD n, DWORD) override {
    if (p && n != sizeof(WINBOOL))
      return D3DERR_INVALIDCALL;
    if (p)
      *static_cast<WINBOOL *>(p) = TRUE;
    return D3D_OK;
  }
};
class SwapChain : public Ref<IDirect3DSwapChain9Stub> {
  Device *device;

public:
  explicit SwapChain(Device *d) : device(d) { d->AddRef(); }
  ~SwapChain() { device->Release(); }
  HRESULT STDMETHODCALLTYPE Present(const RECT *a, const RECT *b, HWND w,
                                    const RGNDATA *r, DWORD) override {
    return device->Present(a, b, w, r);
  }
  HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT i, D3DBACKBUFFER_TYPE t,
                                          IDirect3DSurface9 **p) override {
    return device->GetBackBuffer(0, i, t, p);
  }
  HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS *p) override {
    return device->GetRasterStatus(0, p);
  }
  HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE *p) override {
    return device->GetDisplayMode(0, p);
  }
  HRESULT STDMETHODCALLTYPE
  GetPresentParameters(D3DPRESENT_PARAMETERS *p) override {
    if (!p)
      return E_POINTER;
    *p = device->parameters;
    return D3D_OK;
  }
};
HRESULT Device::CreateQuery(D3DQUERYTYPE type, IDirect3DQuery9 **out) {
  if (out)
    *out = nullptr;
  if (type != D3DQUERYTYPE_EVENT)
    return D3DERR_NOTAVAILABLE;
  if (out)
    *out = new Query(renderer);
  return D3D_OK;
}
HRESULT Device::GetSwapChain(UINT index, IDirect3DSwapChain9 **out) {
  if (!out || index)
    return D3DERR_INVALIDCALL;
  *out = new SwapChain(this);
  return D3D_OK;
}
HRESULT Device::ColorFill(IDirect3DSurface9 *s, const RECT *rect, D3DCOLOR c) {
  if (!s)
    return D3DERR_INVALIDCALL;
  float rgba[] = {float((c >> 16) & 255) / 255, float((c >> 8) & 255) / 255,
                  float(c & 255) / 255, float(c >> 24) / 255};
  uint32_t r[4]{};
  if (rect) {
    r[0] = rect->left;
    r[1] = rect->top;
    r[2] = rect->right - rect->left;
    r[3] = rect->bottom - rect->top;
  }
  renderer->clear(surfaceImage(s), {}, 1, rgba, 1, 0, rect ? r : nullptr);
  return D3D_OK;
}
class Adapter : public Ref<IDirect3D9Stub> {
public:
  UINT STDMETHODCALLTYPE GetAdapterCount() override { return 1; }
  HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void *) override {
    return D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE
  GetAdapterIdentifier(UINT, DWORD, D3DADAPTER_IDENTIFIER9 *p) override {
    if (!p)
      return E_POINTER;
    *p = {};
    std::strcpy(p->Driver, "Metal");
    std::strcpy(p->Description, "Apple Metal native renderer");
    std::strcpy(p->DeviceName, "Display");
    p->VendorId = 0x106b;
    p->WHQLLevel = 1;
    return D3D_OK;
  }
  UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT, D3DFORMAT) override {
    return 1;
  }
  HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT, D3DFORMAT f, UINT n,
                                             D3DDISPLAYMODE *p) override {
    if (n || !p)
      return D3DERR_INVALIDCALL;
    *p = {renderWidth, renderHeight, 60, f};
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT,
                                                  D3DDISPLAYMODE *p) override {
    return EnumAdapterModes(0, D3DFMT_X8R8G8B8, 0, p);
  }
  HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT, D3DDEVTYPE, D3DFORMAT,
                                            D3DFORMAT, WINBOOL) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT, D3DDEVTYPE, D3DFORMAT,
                                              DWORD, D3DRESOURCETYPE type,
                                              D3DFORMAT f) override {
    if (type == D3DRTYPE_VOLUMETEXTURE)
      return D3DERR_NOTAVAILABLE;
    switch (f) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A8B8G8R8:
    case D3DFMT_R5G6B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_A8:
    case D3DFMT_L8:
    case D3DFMT_A8L8:
    case D3DFMT_DXT1:
    case D3DFMT_DXT3:
    case D3DFMT_DXT5:
    case D3DFMT_D16:
    case D3DFMT_D24X8:
    case D3DFMT_D24S8:
    case D3DFMT_D32:
      return D3D_OK;
    default:
      return D3DERR_NOTAVAILABLE;
    }
  }
  HRESULT STDMETHODCALLTYPE
  CheckDeviceMultiSampleType(UINT, D3DDEVTYPE, D3DFORMAT, WINBOOL,
                             D3DMULTISAMPLE_TYPE t, DWORD *quality) override {
    if (quality)
      *quality = t == D3DMULTISAMPLE_NONE ? 1 : 0;
    return t == D3DMULTISAMPLE_NONE ? D3D_OK : D3DERR_NOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT, D3DDEVTYPE, D3DFORMAT,
                                                   D3DFORMAT,
                                                   D3DFORMAT) override {
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT, D3DDEVTYPE,
                                                        D3DFORMAT,
                                                        D3DFORMAT) override {
    return D3D_OK;
  }
  HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT) override {
    return nullptr;
  }
  HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT, D3DDEVTYPE,
                                          D3DCAPS9 *) override;
  HRESULT STDMETHODCALLTYPE CreateDevice(UINT, D3DDEVTYPE, HWND, DWORD,
                                         D3DPRESENT_PARAMETERS *p,
                                         IDirect3DDevice9 **out) override {
    if (!p || !out)
      return D3DERR_INVALIDCALL;
    *out = new Device(this, *p);
    return D3D_OK;
  }
};
HRESULT Adapter::GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType,
                               D3DCAPS9 *pCaps) {
  if (!pCaps)
    return D3DERR_INVALIDCALL;
  std::memset(pCaps, 0, sizeof(*pCaps));
  pCaps->DeviceType = DeviceType;
  pCaps->AdapterOrdinal = Adapter;
  pCaps->Caps2 = D3DCAPS2_CANRENDERWINDOWED | D3DCAPS2_FULLSCREENGAMMA;
  pCaps->PresentationIntervals =
      D3DPRESENT_INTERVAL_IMMEDIATE | D3DPRESENT_INTERVAL_ONE;
  pCaps->DevCaps =
      D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_EXECUTEVIDEOMEMORY |
      D3DDEVCAPS_TLVERTEXSYSTEMMEMORY | D3DDEVCAPS_TLVERTEXVIDEOMEMORY |
      D3DDEVCAPS_HWTRANSFORMANDLIGHT | D3DDEVCAPS_CANRENDERAFTERFLIP;
  pCaps->PrimitiveMiscCaps =
      D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW |
      D3DPMISCCAPS_COLORWRITEENABLE | D3DPMISCCAPS_BLENDOP;
  pCaps->RasterCaps = D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_FOGVERTEX |
                      D3DPRASTERCAPS_FOGTABLE | D3DPRASTERCAPS_MIPMAPLODBIAS |
                      D3DPRASTERCAPS_SCISSORTEST | D3DPRASTERCAPS_DEPTHBIAS |
                      D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS;
  pCaps->ZCmpCaps = 0xFF;
  pCaps->SrcBlendCaps = 0x1FFF;
  pCaps->DestBlendCaps = 0x1FFF;
  pCaps->AlphaCmpCaps = 0xFF;
  pCaps->ShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB |
                     D3DPSHADECAPS_SPECULARGOURAUDRGB |
                     D3DPSHADECAPS_ALPHAGOURAUDBLEND | D3DPSHADECAPS_FOGGOURAUD;
  pCaps->TextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_POW2 |
                       D3DPTEXTURECAPS_NONPOW2CONDITIONAL |
                       D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_CUBEMAP |
                       D3DPTEXTURECAPS_MIPMAP | D3DPTEXTURECAPS_MIPCUBEMAP |
                       D3DPTEXTURECAPS_PROJECTED;
  pCaps->CubeTextureFilterCaps = pCaps->TextureFilterCaps =
      D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR |
      D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR |
      D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR |
      D3DPTFILTERCAPS_MINFANISOTROPIC | D3DPTFILTERCAPS_MAGFANISOTROPIC;
  pCaps->TextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR |
                              D3DPTADDRESSCAPS_CLAMP |
                              D3DPTADDRESSCAPS_INDEPENDENTUV;
  pCaps->MaxTextureWidth = 2048;
  pCaps->MaxTextureHeight = 2048;
  pCaps->MaxVolumeExtent = 0;
  pCaps->MaxTextureRepeat = 8192;
  pCaps->MaxTextureAspectRatio = 2048;
  // Same as DXVK/Windows drivers. NFSU2 derives its world-texture filtering
  // from this value: with MaxAnisotropy = 1 it explicitly selects POINT min/mag
  // filtering (blocky roads).
  pCaps->MaxAnisotropy = 16;
  pCaps->MaxVertexW = 1e10f;
  pCaps->GuardBandLeft = -8192.0f;
  pCaps->GuardBandTop = -8192.0f;
  pCaps->GuardBandRight = 8192.0f;
  pCaps->GuardBandBottom = 8192.0f;
  pCaps->StencilCaps = 0xFF | D3DSTENCILCAPS_TWOSIDED;
  pCaps->FVFCaps = 8;
  pCaps->TextureOpCaps = 0x03FFFFFF & ~(D3DTEXOPCAPS_BUMPENVMAP |
                                        D3DTEXOPCAPS_BUMPENVMAPLUMINANCE);
  pCaps->MaxTextureBlendStages = 8;
  pCaps->MaxSimultaneousTextures = 8;
  pCaps->VertexProcessingCaps = D3DVTXPCAPS_TEXGEN |
                                D3DVTXPCAPS_DIRECTIONALLIGHTS |
                                D3DVTXPCAPS_LOCALVIEWER;
  pCaps->MaxActiveLights = 8;
  pCaps->MaxUserClipPlanes = 0;
  pCaps->MaxVertexBlendMatrices = 0;
  pCaps->MaxPrimitiveCount = 0xFFFFF;
  pCaps->MaxVertexIndex = 0xFFFFFF;
  pCaps->MaxStreams = 16;
  pCaps->MaxStreamStride = 256;
  pCaps->VertexShaderVersion = D3DVS_VERSION(3, 0);
  pCaps->MaxVertexShaderConst = 256;
  pCaps->PixelShaderVersion = D3DPS_VERSION(3, 0);
  pCaps->PixelShader1xMaxValue = 8.0f;
  pCaps->DeclTypes = D3DDTCAPS_UBYTE4 | D3DDTCAPS_UBYTE4N | D3DDTCAPS_SHORT2N |
                     D3DDTCAPS_SHORT4N | D3DDTCAPS_USHORT2N |
                     D3DDTCAPS_USHORT4N;
  pCaps->NumSimultaneousRTs = 1;
  pCaps->MaxPointSize = 64.0f;
  return D3D_OK;
}

Device::Device(Adapter *a, D3DPRESENT_PARAMETERS p) : adapter(a) {
  adapter->AddRef();
  renderer = std::make_shared<Renderer>();
  renderer->attach(hostWindow);
  currentDevice = this;
  reset(&p);
}
Device::~Device() {
  renderer->wait();
  currentDevice = nullptr;
  adapter->Release();
}
HRESULT Device::GetDirect3D(IDirect3D9 **out) {
  if (!out)
    return E_POINTER;
  *out = adapter;
  adapter->AddRef();
  return D3D_OK;
}
HRESULT Device::GetDeviceCaps(D3DCAPS9 *p) {
  return adapter->GetDeviceCaps(0, D3DDEVTYPE_HAL, p);
}
static std::shared_ptr<TextureData> textureData(IDirect3DBaseTexture9 *p) {
  if (auto t = dynamic_cast<Texture2D *>(p))
    return t->data;
  if (auto t = dynamic_cast<TextureCube *>(p))
    return t->data;
  return {};
}
static std::shared_ptr<Image> surfaceImage(IDirect3DSurface9 *p) {
  auto s = dynamic_cast<Surface *>(p);
  if (!s)
    return {};
  return s->view;
}
HRESULT Device::reset(D3DPRESENT_PARAMETERS *p) {
  if (!p)
    return E_POINTER;
  renderer->wait();
  parameters = *p;
  if (!parameters.BackBufferWidth)
    parameters.BackBufferWidth = renderWidth;
  if (!parameters.BackBufferHeight)
    parameters.BackBufferHeight = renderHeight;
  state = {};
  auto w = parameters.BackBufferWidth, h = parameters.BackBufferHeight;
  back = adopt<IDirect3DSurface9>(new Surface(std::make_shared<TextureData>(
      w, h, 1, 1,
      parameters.BackBufferFormat == D3DFMT_UNKNOWN
          ? D3DFMT_A8R8G8B8
          : parameters.BackBufferFormat,
      D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, renderer)));
  state.target = back;
  if (parameters.EnableAutoDepthStencil)
    initialDepth =
        adopt<IDirect3DSurface9>(new Surface(std::make_shared<TextureData>(
            w, h, 1, 1, parameters.AutoDepthStencilFormat,
            D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, renderer)));
  else
    initialDepth.reset();
  state.depth = initialDepth;
  state.viewport = {0, 0, w, h, 0, 1};
  state.scissor = {0, 0, LONG(w), LONG(h)};
  auto &r = state.rs;
  r[D3DRS_TEXTUREFACTOR] = 0xffffffff;
  r[D3DRS_BLENDFACTOR] = 0xffffffff;
  r[D3DRS_FOGEND] = 0x3f800000;
  r[D3DRS_FOGDENSITY] = 0x3f800000;
  r[D3DRS_ZENABLE] = parameters.EnableAutoDepthStencil;
  r[D3DRS_ZWRITEENABLE] = TRUE;
  r[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
  r[D3DRS_CULLMODE] = D3DCULL_CCW;
  r[D3DRS_FILLMODE] = D3DFILL_SOLID;
  r[D3DRS_SRCBLEND] = D3DBLEND_ONE;
  r[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
  r[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
  r[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
  r[D3DRS_BLENDOP] = r[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;
  r[D3DRS_COLORWRITEENABLE] = 15;
  r[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
  r[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
  r[D3DRS_STENCILFAIL] = r[D3DRS_STENCILZFAIL] = r[D3DRS_STENCILPASS] =
      D3DSTENCILOP_KEEP;
  r[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;
  r[D3DRS_CCW_STENCILFAIL] = r[D3DRS_CCW_STENCILZFAIL] =
      r[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
  r[D3DRS_STENCILMASK] = r[D3DRS_STENCILWRITEMASK] = ~0u;
  for (auto &s : state.ss) {
    s[D3DSAMP_ADDRESSU] = s[D3DSAMP_ADDRESSV] = s[D3DSAMP_ADDRESSW] =
        D3DTADDRESS_WRAP;
    s[D3DSAMP_MINFILTER] = s[D3DSAMP_MAGFILTER] = D3DTEXF_POINT;
    s[D3DSAMP_MAXANISOTROPY] = 1;
  }
  for (unsigned i = 0; i < 8; ++i) {
    state.tss[i][D3DTSS_TEXCOORDINDEX] = i;
    state.tss[i][D3DTSS_RESULTARG] = D3DTA_CURRENT;
    state.tss[i][D3DTSS_COLORARG0] = state.tss[i][D3DTSS_ALPHAARG0] =
        D3DTA_CURRENT;
    state.tss[i][D3DTSS_COLOROP] = i ? D3DTOP_DISABLE : D3DTOP_MODULATE;
    state.tss[i][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
    state.tss[i][D3DTSS_COLORARG2] = D3DTA_CURRENT;
    state.tss[i][D3DTSS_ALPHAOP] = i ? D3DTOP_DISABLE : D3DTOP_SELECTARG1;
    state.tss[i][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
    state.tss[i][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
  }
  for (auto key : {D3DTS_WORLD, D3DTS_VIEW, D3DTS_PROJECTION}) {
    D3DMATRIX m{};
    for (unsigned i = 0; i < 4; ++i)
      m.m[i][i] = 1;
    state.transforms[key] = m;
  }
  renderer->resize(w, h);
  return D3D_OK;
}
HRESULT Device::CreateTexture(UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f,
                              D3DPOOL p, IDirect3DTexture9 **out, HANDLE *) {
  if (!out)
    return E_POINTER;
  *out = new Texture2D(
      std::make_shared<TextureData>(w, h, l, 1, f, u, p, renderer));
  return D3D_OK;
}
HRESULT Device::CreateCubeTexture(UINT w, UINT l, DWORD u, D3DFORMAT f,
                                  D3DPOOL p, IDirect3DCubeTexture9 **out,
                                  HANDLE *) {
  if (!out)
    return E_POINTER;
  *out = new TextureCube(
      std::make_shared<TextureData>(w, w, l, 6, f, u, p, renderer));
  return D3D_OK;
}
HRESULT Device::CreateVertexBuffer(UINT n, DWORD u, DWORD f, D3DPOOL p,
                                   IDirect3DVertexBuffer9 **out, HANDLE *) {
  if (!out)
    return E_POINTER;
  *out = new VertexBuffer(n, u, f, D3DFMT_VERTEXDATA, p);
  return D3D_OK;
}
HRESULT Device::CreateIndexBuffer(UINT n, DWORD u, D3DFORMAT f, D3DPOOL p,
                                  IDirect3DIndexBuffer9 **out, HANDLE *) {
  if (!out || !(f == D3DFMT_INDEX16 || f == D3DFMT_INDEX32))
    return D3DERR_INVALIDCALL;
  *out = new IndexBuffer(n, u, 0, f, p);
  return D3D_OK;
}
HRESULT Device::CreateRenderTarget(UINT w, UINT h, D3DFORMAT f,
                                   D3DMULTISAMPLE_TYPE ms, DWORD, WINBOOL,
                                   IDirect3DSurface9 **out, HANDLE *) {
  if (!out || ms != D3DMULTISAMPLE_NONE)
    return D3DERR_INVALIDCALL;
  *out = new Surface(std::make_shared<TextureData>(
      w, h, 1, 1, f, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, renderer));
  return D3D_OK;
}
HRESULT Device::CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT f,
                                          D3DMULTISAMPLE_TYPE ms, DWORD,
                                          WINBOOL, IDirect3DSurface9 **out,
                                          HANDLE *) {
  if (!out || ms != D3DMULTISAMPLE_NONE)
    return D3DERR_INVALIDCALL;
  *out = new Surface(std::make_shared<TextureData>(
      w, h, 1, 1, f, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, renderer));
  return D3D_OK;
}
HRESULT Device::CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT f,
                                            D3DPOOL p, IDirect3DSurface9 **out,
                                            HANDLE *) {
  if (!out)
    return E_POINTER;
  *out =
      new Surface(std::make_shared<TextureData>(w, h, 1, 1, f, 0, p, renderer));
  return D3D_OK;
}
HRESULT Device::Clear(DWORD count, const D3DRECT *rects, DWORD flags,
                      D3DCOLOR c, float z, DWORD st) {
  if (count && !rects)
    return D3DERR_INVALIDCALL;
  float rgba[] = {float((c >> 16) & 255) / 255, float((c >> 8) & 255) / 255,
                  float(c & 255) / 255, float(c >> 24) / 255};
  auto color = surfaceImage(state.target.get()),
       depth = surfaceImage(state.depth.get());
  auto vp = state.viewport;
  if (!count && vp.X == 0 && vp.Y == 0 && color && vp.Width == color->width &&
      vp.Height == color->height) {
    renderer->clear(color, depth, flags, rgba, z, st);
    return D3D_OK;
  }
  for (unsigned i = 0; i < std::max(1u, count); ++i) {
    int left = vp.X, top = vp.Y, right = vp.X + vp.Width,
        bottom = vp.Y + vp.Height;
    if (count) {
      left = std::max(left, rects[i].x1);
      top = std::max(top, rects[i].y1);
      right = std::min(right, rects[i].x2);
      bottom = std::min(bottom, rects[i].y2);
    }
    if (right > left && bottom > top) {
      uint32_t r[] = {uint32_t(left), uint32_t(top), uint32_t(right - left),
                      uint32_t(bottom - top)};
      renderer->clear(color, depth, flags, rgba, z, st, r);
    }
  }
  return D3D_OK;
}

struct ParticleCounters {
  uint64_t candidates = 0, flat = 0, indexed = 0, strips = 0,
           corrected = 0, vertices = 0;
};
static ParticleCounters particleCounters;
static DebugView debugMode(){static const auto mode=parseDebugView(std::getenv("FO2_DEBUG_VIEW"));return mode;}
static bool debugMaterials(){static const bool enabled=!(std::getenv("FO2_SSR_MATERIALS")&&std::string(std::getenv("FO2_SSR_MATERIALS"))=="0");return enabled;}
static void captureMaterial(const Draw& d){
  if(!fo2MetalRaceActive()||!debugMaterials()||(debugMode()!=DebugView::SSR&&debugMode()!=DebugView::SSRMask))return;
  const uint32_t cueBit=1u<<unsigned(d.material.cue);
  if(d.ps->capturedMaterialCues&cueBit)return;
  d.ps->capturedMaterialCues|=cueBit;
  static std::set<std::string> seen;
  auto key=pipelineShaderIdentity(*d.ps)+":"+std::to_string(unsigned(d.material.cue));
  if(seen.size()>=256||!seen.insert(key).second)return;
  const char* session=std::getenv("FO2_METAL_SESSION_DIR");if(!session)return;
  auto folder=std::filesystem::path(session);auto csv=folder/"materials.csv";
  const bool header=!std::filesystem::exists(csv);std::ofstream file(csv,std::ios::app);
  if(header)file<<"shader_hash,entry,cue,reflectivity,roughness,samplers,cube_samplers\n";
  file<<d.ps->hash<<','<<d.ps->entry<<','<<unsigned(d.material.cue)<<','<<d.material.reflectivity<<','<<d.material.roughness<<','<<d.ps->samplerMask<<','<<d.ps->cubeSamplerMask<<'\n';
  std::filesystem::create_directories(folder/"material-shaders");
  std::ofstream(folder/"material-shaders"/(std::to_string(d.ps->hash)+".metal"))<<d.ps->source;
}
HRESULT Device::Present(const RECT *, const RECT *, HWND, const RGNDATA *) {
  auto image = surfaceImage(back.get());
  bool debugReady=false;
  if(debugMode()!=DebugView::Off && fo2MetalRaceActive()){
    const bool ssr=debugMode()==DebugView::SSR||debugMode()==DebugView::SSRMask;
    auto view=ssr?renderer->screenReflections(image,debugMode()==DebugView::SSRMask,debugMode()==DebugView::SSRMask||fo2MetalSSREnabled()):renderer->debugView(debugMode());
    if(view){image=view;debugReady=true;}
  }
  fo2MetalSetDebugReady(debugReady);
  if(debugReady && presentCount.load()%60==0){
    if(const char* path=std::getenv("FO2_DEBUG_STATUS")){
      std::ofstream f(path);f << "{\"ready\":true,\"mode\":\"" << debugViewName(debugMode())
        << "\",\"frame\":" << presentCount.load() << ",\"width\":" << image->width << ",\"height\":" << image->height << "}\n";
    }
  }
  static auto start = std::chrono::steady_clock::now();
  static unsigned frames = 0;
  static char label[80] = "METAL";
  ++frames;
  auto now = std::chrono::steady_clock::now();
  double elapsed = std::chrono::duration<double>(now - start).count();
  if (elapsed >= 0.5) {
    std::snprintf(label, sizeof(label), "METAL %.1f FPS | %.2f ms",
                  frames / elapsed, elapsed * 1000 / frames);
    frames = 0;
    start = now;
  }
  std::string overlay=showFps.load()?label:"";
  if(debugMode()!=DebugView::Off)overlay += std::string(" | ")+(debugMode()==DebugView::Depth?"DEPTH":debugMode()==DebugView::Normals?"GEOMETRIC NORMALS":debugMode()==DebugView::SSRMask?(debugMaterials()?"SSR MATERIAL MASK":"SSR GEOMETRIC MASK"):fo2MetalSSREnabled()?"SSR ON [F8]":"SSR OFF [F8]")+
    (debugReady?"":(fo2MetalRaceActive()?" WAITING FOR DEPTH":" WAITING FOR RACE"));
  if(debugMode()==DebugView::SSR&&fo2MetalSSREnabled())overlay+=fo2MetalWaterTraceLabel();
  renderer->overlay(image, overlay.c_str());
  if (screenshot.exchange(false)) {
    auto pixels = renderer->read(image);
    const char* captureDir=std::getenv("FO2_SCREENSHOT_DIR");
    const std::string folder=captureDir&&*captureDir?captureDir:"work/metal/captures";
    std::filesystem::create_directories(folder);
    std::ofstream file(folder+"/frame-" +
                           std::to_string(presentCount.load()) + ".ppm",
                       std::ios::binary);
    file << "P6\n" << image->width << ' ' << image->height << "\n255\n";
    for (size_t i = 0; i < pixels.size(); i += 4)
      file.write(reinterpret_cast<char *>(pixels.data() + i), 3);
    if(debugMode()==DebugView::SSR||debugMode()==DebugView::SSRMask)
      renderer->captureReflectionDiagnostics(folder+"/ssr-inspect",surfaceImage(back.get()));
  }
  renderer->present(image);
  const auto frame = ++presentCount;
  if (frame % 300 == 0) {
    const auto &p = particleCounters;
    std::printf("[FO2 METAL PARTICLES] frame=%u candidates=%llu flat=%llu indexed=%llu strips=%llu corrected=%llu vertices=%llu\n",
        unsigned(frame), (unsigned long long)p.candidates, (unsigned long long)p.flat,
        (unsigned long long)p.indexed, (unsigned long long)p.strips,
        (unsigned long long)p.corrected, (unsigned long long)p.vertices);
    particleCounters = {};
  }
  return D3D_OK;
}
HRESULT Device::StretchRect(IDirect3DSurface9 *from, const RECT *sr,
                            IDirect3DSurface9 *to, const RECT *dr,
                            D3DTEXTUREFILTERTYPE filter) {
  if (!from || !to)
    return D3DERR_INVALIDCALL;
  uint32_t a[4]{}, b[4]{};
  auto convert = [](const RECT *r, uint32_t *v) {
    if (!r)
      return;
    v[0] = r->left;
    v[1] = r->top;
    v[2] = r->right - r->left;
    v[3] = r->bottom - r->top;
  };
  convert(sr, a);
  convert(dr, b);
  renderer->copy(surfaceImage(from), surfaceImage(to), sr ? a : nullptr,
                 dr ? b : nullptr, filter == D3DTEXF_LINEAR);
  return D3D_OK;
}
HRESULT Device::UpdateSurface(IDirect3DSurface9 *src, const RECT *rect,
                              IDirect3DSurface9 *dst, const POINT *point) {
  auto a = dynamic_cast<Surface *>(src), b = dynamic_cast<Surface *>(dst);
  if (!a || !b || a->data->format != b->data->format)
    return D3DERR_INVALIDCALL;
  D3DLOCKED_RECT x{}, y{};
  RECT r = rect ? *rect
                : RECT{0, 0, LONG(a->data->width(a->level)),
                       LONG(a->data->height(a->level))};
  RECT d{point ? point->x : 0, point ? point->y : 0, 0, 0};
  d.right = d.left + r.right - r.left;
  d.bottom = d.top + r.bottom - r.top;
  auto hr = a->LockRect(&x, &r, D3DLOCK_READONLY);
  if (FAILED(hr))
    return hr;
  hr = b->LockRect(&y, &d, 0);
  if (SUCCEEDED(hr)) {
    unsigned rows = compressed(a->data->format) ? (r.bottom - r.top + 3) / 4
                                                : r.bottom - r.top;
    unsigned bytes =
        compressed(a->data->format)
            ? ((r.right - r.left + 3) / 4) *
                  (a->data->format == D3DFMT_DXT1 ? 8 : 16)
            : (r.right - r.left) * d3dFormatBytesPerPixel(a->data->format);
    for (unsigned i = 0; i < rows; ++i)
      std::memcpy(static_cast<char *>(y.pBits) + i * y.Pitch,
                  static_cast<char *>(x.pBits) + i * x.Pitch, bytes);
    b->UnlockRect();
  }
  a->UnlockRect();
  return hr;
}
HRESULT Device::UpdateTexture(IDirect3DBaseTexture9 *src,
                              IDirect3DBaseTexture9 *dst) {
  auto a = textureData(src), b = textureData(dst);
  if (!a || !b || a->raw.size() != b->raw.size() || a->format != b->format ||
      a->image->width != b->image->width ||
      a->image->height != b->image->height)
    return D3DERR_INVALIDCALL;
  b->raw = a->raw;
  b->image->rgba = a->image->rgba;
  ++b->image->version;
  return D3D_OK;
}
HRESULT Device::GetRenderTargetData(IDirect3DSurface9 *src,
                                    IDirect3DSurface9 *dst) {
  auto a = dynamic_cast<Surface *>(src), b = dynamic_cast<Surface *>(dst);
  if (!a || !b || a->view->width != b->view->width ||
      a->view->height != b->view->height)
    return D3DERR_INVALIDCALL;
  b->data->storeRGBA(b->face, b->level, renderer->read(a->view));
  ++b->data->image->version;
  return D3D_OK;
}

HRESULT Device::MultiplyTransform(D3DTRANSFORMSTATETYPE t, const D3DMATRIX *p) {
  if (!p)
    return E_POINTER;
  auto a = target().transforms[t];
  D3DMATRIX r{};
  for (unsigned i = 0; i < 4; ++i)
    for (unsigned j = 0; j < 4; ++j)
      for (unsigned k = 0; k < 4; ++k)
        r.m[i][j] += a.m[i][k] * p->m[k][j];
  return SetTransform(t, &r);
}
void Device::apply(const State &s, const Mask &m) {
  for (unsigned i = 0; i < 256; ++i) {
    if (m.rs[i])
      state.rs[i] = s.rs[i];
    if (m.vf[i])
      std::copy_n(s.vsF.data() + 4 * i, 4, state.vsF.data() + 4 * i);
    if (m.pf[i])
      std::copy_n(s.psF.data() + 4 * i, 4, state.psF.data() + 4 * i);
  }
  for (unsigned i = 0; i < 16; ++i) {
    for (unsigned j = 0; j < 16; ++j)
      if (m.ss[i][j])
        state.ss[i][j] = s.ss[i][j];
    if (m.textures[i])
      state.textures[i] = s.textures[i];
    if (m.streams[i]) {
      state.streams[i] = s.streams[i];
      state.strides[i] = s.strides[i];
      state.offsets[i] = s.offsets[i];
      state.frequencies[i] = s.frequencies[i];
    }
    if (m.vi[i])
      std::copy_n(s.vsI.data() + 4 * i, 4, state.vsI.data() + 4 * i);
    if (m.pi[i])
      std::copy_n(s.psI.data() + 4 * i, 4, state.psI.data() + 4 * i);
    if (m.vb[i])
      state.vsB[i] = s.vsB[i];
    if (m.pb[i])
      state.psB[i] = s.psB[i];
  }
  for (unsigned i = 0; i < 8; ++i)
    for (unsigned j = 0; j < 33; ++j)
      if (m.tss[i][j])
        state.tss[i][j] = s.tss[i][j];
  if (m.vs)
    state.vs = s.vs;
  if (m.ps)
    state.ps = s.ps;
  if (m.decl)
    state.declaration = s.declaration;
  if (m.fvf)
    state.fvf = s.fvf;
  if (m.indices)
    state.indices = s.indices;
  if (m.viewport)
    state.viewport = s.viewport;
  if (m.scissor)
    state.scissor = s.scissor;
  if (m.material)
    state.material = s.material;
  for (auto k : m.transforms)
    state.transforms[k] = s.transforms.at(k);
}
Mask Device::allMask(D3DSTATEBLOCKTYPE type) {
  Mask m;
  m.rs.set();
  m.textures.set();
  for (auto &b : m.ss)
    b.set();
  for (auto &b : m.tss)
    b.set();
  if (type != D3DSBT_PIXELSTATE) {
    m.vs = m.decl = m.fvf = m.viewport = m.scissor = m.indices = m.material =
        true;
    m.streams.set();
    m.vf.set();
    m.vi.set();
    m.vb.set();
    for (auto &t : state.transforms)
      m.transforms.insert(t.first);
  }
  if (type != D3DSBT_VERTEXSTATE) {
    m.ps = true;
    m.pf.set();
    m.pi.set();
    m.pb.set();
  }
  return m;
}
StateBlock::StateBlock(Device *d, State s, Mask m)
    : device(d), state(std::move(s)), mask(m) {
  d->AddRef();
  trim();
}
void StateBlock::trim() {
  for (unsigned i = 0; i < 16; ++i) {
    if (!mask.textures[i])
      state.textures[i].reset();
    if (!mask.streams[i])
      state.streams[i].reset();
  }
  if (!mask.indices)
    state.indices.reset();
  if (!mask.vs)
    state.vs.reset();
  if (!mask.ps)
    state.ps.reset();
  if (!mask.decl)
    state.declaration.reset();
  state.target.reset();
  state.depth.reset();
}
StateBlock::~StateBlock() { device->Release(); }
HRESULT StateBlock::Capture() {
  state = device->state;
  trim();
  return D3D_OK;
}
HRESULT StateBlock::Apply() {
  device->apply(state, mask);
  return D3D_OK;
}
HRESULT Device::EndStateBlock(IDirect3DStateBlock9 **out) {
  if (!out || !recording)
    return D3DERR_INVALIDCALL;
  *out = new StateBlock(this, recordState, recordMask);
  recording = false;
  recordState = {};
  return D3D_OK;
}
HRESULT Device::CreateStateBlock(D3DSTATEBLOCKTYPE t,
                                 IDirect3DStateBlock9 **out) {
  if (!out || t < D3DSBT_ALL || t > D3DSBT_VERTEXSTATE)
    return D3DERR_INVALIDCALL;
  *out = new StateBlock(this, state, allMask(t));
  return D3D_OK;
}
static std::vector<D3DVERTEXELEMENT9> fvfElements(DWORD f) {
  std::vector<D3DVERTEXELEMENT9> e;
  WORD offset = 0;
  auto add = [&](BYTE type, BYTE usage, BYTE idx, unsigned size) {
    e.push_back({0, offset, type, 0, usage, idx});
    offset += size;
  };
  auto pos = f & D3DFVF_POSITION_MASK;
  if (pos == D3DFVF_XYZ)
    add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0, 12);
  else if (pos == D3DFVF_XYZRHW)
    add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITIONT, 0, 16);
  else
    throw std::runtime_error("Unsupported fixed-function position FVF");
  if (f & D3DFVF_NORMAL)
    add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_NORMAL, 0, 12);
  if (f & D3DFVF_PSIZE)
    add(D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_PSIZE, 0, 4);
  if (f & D3DFVF_DIFFUSE)
    add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0, 4);
  if (f & D3DFVF_SPECULAR)
    add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 1, 4);
  unsigned count = (f & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
  for (unsigned i = 0; i < count; ++i) {
    unsigned bits = (f >> (16 + i * 2)) & 3, n = bits == 0   ? 2
                                                 : bits == 1 ? 3
                                                 : bits == 2 ? 4
                                                             : 1;
    add(n - 1, D3DDECLUSAGE_TEXCOORD, i, n * 4);
  }
  return e;
}
static std::shared_ptr<Shader>
fixedVertex(const State &state, const std::vector<D3DVERTEXELEMENT9> &elements,
            bool transformed) {
  std::string s =
      "#include <metal_stdlib>\nusing namespace metal;\nstruct Input {\n";
  unsigned pos = 0;
  int colors[2] = {-1, -1}, uv[8];
  std::fill_n(uv, 8, -1);
  for (unsigned i = 0; i < elements.size(); ++i) {
    s += "float4 v" + std::to_string(i) + " [[attribute(" + std::to_string(i) +
         ")]];\n";
    if (elements[i].Usage == D3DDECLUSAGE_POSITION ||
        elements[i].Usage == D3DDECLUSAGE_POSITIONT)
      pos = i;
    if (elements[i].Usage == D3DDECLUSAGE_COLOR && elements[i].UsageIndex < 2)
      colors[elements[i].UsageIndex] = i;
    if (elements[i].Usage == D3DDECLUSAGE_TEXCOORD &&
        elements[i].UsageIndex < 8)
      uv[elements[i].UsageIndex] = i;
  }
  s += "};struct Output {float4 position [[position]];float4 color0 "
       "[[user(color0)]];float4 color1 [[user(color1)]];float4 fog "
       "[[user(fog)]];";
  for (unsigned i = 0; i < 8; ++i)
    s += "float4 tc" + std::to_string(i) + " [[user(texcoord" +
         std::to_string(i) + ")]];";
  s += "};\nvertex Output fo2_vertex(Input input [[stage_in]],constant float4* "
       "c [[buffer(0)]],constant float* fx [[buffer(30)]]) { Output "
       "output;float4 p=input.v" +
       std::to_string(pos) + ";";
  if (transformed)
    s += "float "
         "w=p.w!=0?1.0/p.w:1.0;output.position=float4(((p.x-fx[16]+0.5)*2.0/"
         "fx[18]-1.0)*w,(1.0-(p.y-fx[17]+0.5)*2.0/fx[19])*w,p.z*w,w);";
  else
    s +=
        "p.w=1;output.position=float4(dot(p,c[0]),dot(p,c[1]),dot(p,c[2]),dot("
        "p,c[3]));output.position.xy+=float2(fx[20],fx[21])*output.position.w;";
  for (unsigned i = 0; i < 2; ++i) {
    int a = colors[i];
    std::string value =
        a < 0 ? (i ? "float4(0)" : "float4(1)")
              : "input.v" + std::to_string(a) +
                    (elements[a].Type == D3DDECLTYPE_D3DCOLOR ? ".zyxw" : "");
    s += "output.color" + std::to_string(i) + "=" + value + ";";
  }
  s += transformed ? "output.fog=float4(output.color1.a);"
                   : "output.fog=float4(1);";
  for (unsigned i = 0; i < 8; ++i) {
    auto index = state.tss[i][D3DTSS_TEXCOORDINDEX];
    if (index >= 8) {
      if (state.tss[i][D3DTSS_COLOROP] != D3DTOP_DISABLE)
        throw std::runtime_error(
            "Generated texture coordinates are unsupported");
      index = 0;
    }
    auto a = uv[index];
    std::string value = a < 0 ? "float4(0)" : "input.v" + std::to_string(a);
    if (state.tss[i][D3DTSS_TEXTURETRANSFORMFLAGS] & 0xff) {
      s += "{float4 t=" + value + ";output.tc" + std::to_string(i) + "=float4(";
      for (unsigned j = 0; j < 4; ++j)
        s += (j ? "," : "") + std::string("dot(t,c[") +
             std::to_string(4 + i * 4 + j) + "])";
      s += ");}";
    } else
      s += "output.tc" + std::to_string(i) + "=" + value + ";";
  }
  s += "return output;}";
  auto shader = std::make_shared<Shader>();
  shader->source = s;
  shader->entry = "fo2_vertex";
  shader->vertex = true;
  shader->hash = shaderHash(s.data(), s.size());
  for (unsigned i = 0; i < elements.size(); ++i)
    shader->inputs.push_back({i, elements[i].Usage, elements[i].UsageIndex});
  return shader;
}
static std::shared_ptr<Shader> fixedPixel(const State &state,
                                          const Shader &vs) {
  std::string fields, aliases, arguments, body;
  uint32_t samplerMask = 0, cubeMask = 0;
  std::map<std::string, std::string> inputFields;
  auto input = [&](std::string field, std::string semantic,
                   std::string fallback) {
    auto found = inputFields.find(semantic);
    if (found != inputFields.end())
      return found->second;
    if (vs.source.find("[[user(" + semantic + ")]]") != std::string::npos) {
      fields += "float4 " + field + " [[user(" + semantic + ")]];";
      return inputFields[semantic] = std::string("input.") + field;
    }
    return fallback;
  };
  auto diffuse = input("diffuse", "color0", "float4(1)"),
       specular = input("specular", "color1", "float4(0)"),
       fog = input("fog", "fog", "float4(1)");
  for (unsigned i = 0; i < 8; ++i) {
    auto &t = state.tss[i];
    if (t[D3DTSS_COLOROP] == D3DTOP_DISABLE)
      break;
    auto texture = textureData(state.textures[i].get());
    FFPStage st{t[D3DTSS_COLOROP],
                t[D3DTSS_COLORARG0],
                t[D3DTSS_COLORARG1],
                t[D3DTSS_COLORARG2],
                t[D3DTSS_ALPHAOP],
                t[D3DTSS_ALPHAARG0],
                t[D3DTSS_ALPHAARG1],
                t[D3DTSS_ALPHAARG2],
                t[D3DTSS_RESULTARG],
                t[D3DTSS_TEXCOORDINDEX],
                t[D3DTSS_TEXTURETRANSFORMFLAGS],
                texture ? (texture->image->faces == 6 ? 2 : 1) : 0};
    unsigned tc = state.vs ? st.texCoordIndex : i;
    if (tc >= 8)
      throw std::runtime_error("Invalid fixed-function texture coordinate");
    auto coord = input("tc" + std::to_string(i),
                       "texcoord" + std::to_string(tc), "float4(0)");
    std::string number = std::to_string(i);
    if (st.texType) {
      samplerMask |= 1u << i;
      if (st.texType == 2)
        cubeMask |= 1u << i;
      arguments += ",texture" + std::string(st.texType == 2 ? "cube" : "2d") +
                   "<float> image" + number + " [[texture(" + number +
                   ")]],sampler sample" + number + " [[sampler(" + number +
                   ")]]";
      std::string uv = coord + (st.texType == 2 ? ".xyz" : ".xy");
      if (st.transformFlags & D3DTTFF_PROJECTED) {
        unsigned n = st.transformFlags & 255;
        uv = "(" + uv + "/" + coord +
             (n == 2   ? ".y"
              : n == 3 ? ".z"
                       : ".w") +
             ")";
      }
      body += "tex=image" + number + ".sample(sample" + number + "," + uv +
              ",bias(fo2fx[" + std::to_string(32 + i) + "]));";
    } else
      body += "tex=float4(1);";
    std::string target = st.resultArg == D3DTA_TEMP ? "tmp" : "cur";
    body +=
        "{float4 stage_constant=c[" + std::to_string(i + 1) +
        "];float3 rgb=" + ffpOp(st.colorOp, st, false) + ";float a=" +
        (st.alphaOp == D3DTOP_DISABLE ? "cur.a" : ffpOp(st.alphaOp, st, true)) +
        ";" + target + "=clamp(float4(rgb,a),0.0,1.0);}";
  }
  std::string source = "#include <metal_stdlib>\nusing namespace metal;\n";
  if (!fields.empty())
    source += "struct Input{" + fields + "};";
  source += "fragment float4 fo2_fragment(";
  if (!fields.empty())
    source += "Input input [[stage_in]],";
  source += "float4 position [[position]],constant float* fo2fx "
            "[[buffer(30)]],constant float4* c [[buffer(0)]]" +
            arguments + "){float4 v_diffuse=" + diffuse +
            ",v_specular=" + specular +
            ",u_tfactor=c[0],cur=v_diffuse,tmp=float4(0),tex=float4(1);" + body;
  if (state.rs[D3DRS_SPECULARENABLE])
    source += "cur.rgb=clamp(cur.rgb+v_specular.rgb,0.0,1.0);";
  source += pixelEffects("cur", "position", fog + ".x") + "return cur;}";
  auto shader = std::make_shared<Shader>();
  shader->source = source;
  shader->samplerMask = samplerMask;
  shader->cubeSamplerMask = cubeMask;
  shader->entry = "fo2_fragment";
  shader->hash = shaderHash(source.data(), source.size());
  return shader;
}
static Blend blendValue(DWORD v) {
  switch (v) {
  case D3DBLEND_ZERO:
    return Blend::Zero;
  case D3DBLEND_ONE:
    return Blend::One;
  case D3DBLEND_SRCCOLOR:
    return Blend::SrcColor;
  case D3DBLEND_INVSRCCOLOR:
    return Blend::InvSrcColor;
  case D3DBLEND_SRCALPHA:
    return Blend::SrcAlpha;
  case D3DBLEND_INVSRCALPHA:
    return Blend::InvSrcAlpha;
  case D3DBLEND_DESTALPHA:
    return Blend::DstAlpha;
  case D3DBLEND_INVDESTALPHA:
    return Blend::InvDstAlpha;
  case D3DBLEND_DESTCOLOR:
    return Blend::DstColor;
  case D3DBLEND_INVDESTCOLOR:
    return Blend::InvDstColor;
  case D3DBLEND_SRCALPHASAT:
    return Blend::SrcAlphaSat;
  case D3DBLEND_BLENDFACTOR:
    return Blend::Constant;
  case D3DBLEND_INVBLENDFACTOR:
    return Blend::InvConstant;
  default:
    throw std::runtime_error("Unsupported Metal blend factor");
  }
}
Draw Device::prepare() {
  Draw d;
  d.captureMaterial=true;
  d.color = surfaceImage(state.target.get());
  d.depth = surfaceImage(state.depth.get());
  if (!d.color)
    throw std::runtime_error("No color render target");
  std::vector<D3DVERTEXELEMENT9> elements;
  if (auto dec = dynamic_cast<Declaration *>(state.declaration.get())) {
    for (auto e : dec->elements)
      if (e.Stream != 0xff)
        elements.push_back(e);
  } else
    elements = fvfElements(state.fvf);
  uint32_t colors = 0, cubes = 0;
  for (unsigned i = 0; i < 16; ++i) {
    auto tex = textureData(state.textures[i].get());
    if (tex) {
      d.textures[i] = tex->image;
      if (tex->image->faces == 6)
        cubes |= 1u << i;
    }
    auto &sm = d.samplers[i];
    auto &ss = state.ss[i];
    sm.address[0] = ss[D3DSAMP_ADDRESSU];
    sm.address[1] = ss[D3DSAMP_ADDRESSV];
    sm.address[2] = ss[D3DSAMP_ADDRESSW];
    sm.minLinear = ss[D3DSAMP_MINFILTER] != D3DTEXF_POINT;
    sm.magLinear = ss[D3DSAMP_MAGFILTER] != D3DTEXF_POINT;
    sm.mipLinear = ss[D3DSAMP_MIPFILTER] == D3DTEXF_LINEAR;
    sm.mipEnabled = ss[D3DSAMP_MIPFILTER] != D3DTEXF_NONE;
    sm.minLod = ss[D3DSAMP_MAXMIPLEVEL];
    sm.maxAnisotropy = ss[D3DSAMP_MAXANISOTROPY];
    sm.lodBias = asFloat(ss[D3DSAMP_MIPMAPLODBIAS]);
    d.effects[32 + i] = sm.lodBias;
    if (ss[D3DSAMP_SRGBTEXTURE])
      d.textureSrgbMask |= 1u << i;
    if (auto b = dynamic_cast<VertexBuffer *>(state.streams[i].get())) {
      d.streams[i] = b->data;
      d.strides[i] = state.strides[i];
      d.offsets[i] = state.offsets[i];
      if (state.frequencies[i] & D3DSTREAMSOURCE_INDEXEDDATA)
        d.instanceCount = state.frequencies[i] & 0x3fffffff;
      if (state.frequencies[i] & D3DSTREAMSOURCE_INSTANCEDATA) {
        d.perInstanceMask |= 1u << i;
        d.stepRates[i] = state.frequencies[i] & 0x3fffffff;
      }
    }
  }
  if (auto vs = dynamic_cast<VertexShader *>(state.vs.get())) {
    auto plain = vs->translated(0, 0);
    for (auto a : plain->inputs)
      for (auto e : elements)
        if (e.Usage == a[1] && e.UsageIndex == a[2] &&
            e.Type == D3DDECLTYPE_D3DCOLOR)
          colors |= 1u << a[0];
    d.vs = vs->translated(colors, 0);
    d.vsConstants = packConstants(*d.vs, state.vsF.data(), state.vsI.data(),
                                  state.vsB.data());
  } else {
    bool transformed =
        std::any_of(elements.begin(), elements.end(),
                    [](auto e) { return e.Usage == D3DDECLUSAGE_POSITIONT; });
    d.vs = fixedVertex(state, elements, transformed);
    D3DMATRIX w = state.transforms[D3DTS_WORLD],
              v = state.transforms[D3DTS_VIEW],
              p = state.transforms[D3DTS_PROJECTION], wv{}, wvp{};
    for (unsigned i = 0; i < 4; ++i)
      for (unsigned j = 0; j < 4; ++j)
        for (unsigned k = 0; k < 4; ++k)
          wv.m[i][j] += w.m[i][k] * v.m[k][j];
    for (unsigned i = 0; i < 4; ++i)
      for (unsigned j = 0; j < 4; ++j)
        for (unsigned k = 0; k < 4; ++k)
          wvp.m[i][j] += wv.m[i][k] * p.m[k][j];
    float c[144]{};
    for (unsigned i = 0; i < 4; ++i)
      for (unsigned j = 0; j < 4; ++j)
        c[i * 4 + j] = wvp.m[j][i];
    for (unsigned n = 0; n < 8; ++n) {
      auto m = state.transforms[D3DTRANSFORMSTATETYPE(D3DTS_TEXTURE0 + n)];
      for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = 0; j < 4; ++j)
          c[16 + n * 16 + i * 4 + j] = m.m[j][i];
    }
    d.vsConstants.resize(sizeof(c));
    std::memcpy(d.vsConstants.data(), c, sizeof(c));
  }
  if (auto ps = dynamic_cast<PixelShader *>(state.ps.get())) {
    d.ps = ps->translated(
        0, cubes, d.vs->source.find("[[user(fog)]]") != std::string::npos);
    d.psConstants = packConstants(*d.ps, state.psF.data(), state.psI.data(),
                                  state.psB.data());
  } else {
    d.ps = fixedPixel(state, *d.vs);
    float c[36]{};
    auto color = [&](DWORD value, float *dest) {
      dest[0] = float((value >> 16) & 255) / 255;
      dest[1] = float((value >> 8) & 255) / 255;
      dest[2] = float(value & 255) / 255;
      dest[3] = float(value >> 24) / 255;
    };
    color(state.rs[D3DRS_TEXTUREFACTOR], c);
    for (unsigned i = 0; i < 8; ++i)
      color(state.tss[i][D3DTSS_CONSTANT], c + 4 + i * 4);
    d.psConstants.resize(sizeof(c));
    std::memcpy(d.psConstants.data(), c, sizeof(c));
  }
  for (auto a : d.vs->inputs) {
    auto found = std::find_if(elements.begin(), elements.end(), [&](auto e) {
      return (e.Usage == a[1] || (a[1] == D3DDECLUSAGE_POSITION &&
                                  e.Usage == D3DDECLUSAGE_POSITIONT)) &&
             e.UsageIndex == a[2];
    });
    if (found == elements.end())
      throw std::runtime_error("Missing vertex declaration semantic " +
                               std::to_string(a[1]) + ":" +
                               std::to_string(a[2]));
    auto e = *found;
    if (e.Type == D3DDECLTYPE_UDEC3 || e.Type == D3DDECLTYPE_DEC3N ||
        e.Type > D3DDECLTYPE_FLOAT16_4)
      throw std::runtime_error("Unsupported vertex element type");
    d.attributes.push_back(
        {a[0], e.Stream, e.Offset,
         static_cast<Element>(e.Type >= D3DDECLTYPE_FLOAT16_2 ? e.Type - 2
                                                              : e.Type)});
  }
  if (auto ib = dynamic_cast<IndexBuffer *>(state.indices.get())) {
    d.indices = ib->data;
    d.indexSize = ib->format == D3DFMT_INDEX32 ? 4 : 2;
  }
  auto &r = d.raster;
  auto &rs = state.rs;
  d.colorSrgb = rs[D3DRS_SRGBWRITEENABLE];
  auto &vp = state.viewport;
  r.viewport[0] = vp.X;
  r.viewport[1] = vp.Y;
  r.viewport[2] = vp.Width;
  r.viewport[3] = vp.Height;
  r.viewport[4] = vp.MinZ;
  r.viewport[5] = vp.MaxZ;
  r.depthOn = rs[D3DRS_ZENABLE];
  r.depthWrite = rs[D3DRS_ZWRITEENABLE];
  r.depthCompare = Compare(std::max(1u, rs[D3DRS_ZFUNC]) - 1);
  r.cull = rs[D3DRS_CULLMODE] == D3DCULL_CW    ? 1
           : rs[D3DRS_CULLMODE] == D3DCULL_CCW ? 2
                                               : 0;
  r.fill = rs[D3DRS_FILLMODE] != D3DFILL_SOLID;
  r.colorMask = rs[D3DRS_COLORWRITEENABLE];
  r.blendOn = rs[D3DRS_ALPHABLENDENABLE];
  r.src = blendValue(rs[D3DRS_SRCBLEND]);
  r.dst = blendValue(rs[D3DRS_DESTBLEND]);
  r.blend = BlendOp(std::max(1u, rs[D3DRS_BLENDOP]) - 1);
  r.srcAlpha = rs[D3DRS_SEPARATEALPHABLENDENABLE]
                   ? blendValue(rs[D3DRS_SRCBLENDALPHA])
                   : r.src;
  r.dstAlpha = rs[D3DRS_SEPARATEALPHABLENDENABLE]
                   ? blendValue(rs[D3DRS_DESTBLENDALPHA])
                   : r.dst;
  r.blendAlpha = rs[D3DRS_SEPARATEALPHABLENDENABLE]
                     ? BlendOp(std::max(1u, rs[D3DRS_BLENDOPALPHA]) - 1)
                     : r.blend;
  DWORD bc = rs[D3DRS_BLENDFACTOR];
  r.blendColor[0] = float((bc >> 16) & 255) / 255;
  r.blendColor[1] = float((bc >> 8) & 255) / 255;
  r.blendColor[2] = float(bc & 255) / 255;
  r.blendColor[3] = float(bc >> 24) / 255;
  r.scissorOn = rs[D3DRS_SCISSORTESTENABLE];
  r.scissor[0] = std::max(0, state.scissor.left);
  r.scissor[1] = std::max(0, state.scissor.top);
  r.scissor[2] = std::max(0, state.scissor.right - state.scissor.left);
  r.scissor[3] = std::max(0, state.scissor.bottom - state.scissor.top);
  r.stencilOn = rs[D3DRS_STENCILENABLE];
  r.stencilCompare = Compare(std::max(1u, rs[D3DRS_STENCILFUNC]) - 1);
  r.stencilFail = StencilOp(std::max(1u, rs[D3DRS_STENCILFAIL]) - 1);
  r.stencilDepthFail = StencilOp(std::max(1u, rs[D3DRS_STENCILZFAIL]) - 1);
  r.stencilPass = StencilOp(std::max(1u, rs[D3DRS_STENCILPASS]) - 1);
  r.twoSidedStencil = rs[D3DRS_TWOSIDEDSTENCILMODE];
  r.backStencilCompare = Compare(std::max(1u, rs[D3DRS_CCW_STENCILFUNC]) - 1);
  r.backStencilFail = StencilOp(std::max(1u, rs[D3DRS_CCW_STENCILFAIL]) - 1);
  r.backStencilDepthFail = StencilOp(std::max(1u, rs[D3DRS_CCW_STENCILZFAIL]) - 1);
  r.backStencilPass = StencilOp(std::max(1u, rs[D3DRS_CCW_STENCILPASS]) - 1);
  r.stencilRead = rs[D3DRS_STENCILMASK];
  r.stencilWrite = rs[D3DRS_STENCILWRITEMASK];
  r.stencilRef = rs[D3DRS_STENCILREF];
  r.depthBias = asFloat(rs[D3DRS_DEPTHBIAS]);
  r.slopeBias = asFloat(rs[D3DRS_SLOPESCALEDEPTHBIAS]);
  d.effects[0] = rs[D3DRS_ALPHATESTENABLE];
  d.effects[1] = rs[D3DRS_ALPHAFUNC];
  d.effects[2] = float(rs[D3DRS_ALPHAREF] & 255) / 255;
  d.effects[3] = rs[D3DRS_FOGENABLE];
  d.effects[4] = rs[D3DRS_FOGTABLEMODE];
  d.effects[5] = asFloat(rs[D3DRS_FOGSTART]);
  d.effects[6] = asFloat(rs[D3DRS_FOGEND]);
  d.effects[7] = asFloat(rs[D3DRS_FOGDENSITY]);
  DWORD fogColor = rs[D3DRS_FOGCOLOR];
  d.effects[8] = float((fogColor >> 16) & 255) / 255;
  d.effects[9] = float((fogColor >> 8) & 255) / 255;
  d.effects[10] = float(fogColor & 255) / 255;
  d.effects[16] = vp.X;
  d.effects[17] = vp.Y;
  d.effects[18] = vp.Width;
  d.effects[19] = vp.Height;
  d.effects[20] = 1.0f / std::max(1u, vp.Width);
  d.effects[21] = -1.0f / std::max(1u, vp.Height);
  if(auto vs=dynamic_cast<VertexShader*>(state.vs.get());vs&&knownHudVertex(vs->codeHash,vs->bytecode.size()))if(auto ps=dynamic_cast<PixelShader*>(state.ps.get())){
    auto texture=textureData(state.textures[0].get());
    const bool ordinary=texture&&texture->image->faces==1&&
      !(texture->image->width==vp.Width&&texture->image->height==vp.Height);
    d.hudBoundary=knownHudBoundary(vs->codeHash,vs->bytecode.size(),ps->codeHash,ps->bytecode.size(),state.vsF.data()+12,
      r.depthOn,r.blendOn&&r.src==Blend::SrcAlpha&&r.dst==Blend::InvSrcAlpha&&r.blend==BlendOp::Add,
      texture&&bool(texture->usage&(D3DUSAGE_RENDERTARGET|D3DUSAGE_DEPTHSTENCIL)),ordinary);
    if(debugMode()!=DebugView::Off&&fo2MetalRaceActive()){
      const auto w=state.vsF.data()+12;
      const bool ortho=std::abs(w[0])+std::abs(w[1])+std::abs(w[2])<1e-6f&&std::abs(w[3]-1)<1e-6f;
      if(ortho){
        static std::set<uint64_t> logged;
        uint64_t key=(uint64_t(r.src)<<32)|(uint64_t(r.dst)<<24)|(uint64_t(r.depthOn)<<16)|(uint64_t(r.blendOn)<<15)|
          (uint64_t(texture?texture->usage:0)<<1)|uint64_t(d.hudBoundary);
        if(logged.size()<16&&logged.insert(key).second)
          std::printf("[FO2 HUD BOUNDARY] accepted=%d vs=%016llx ps=%016llx depth=%d blend=%d/%u/%u texture=%ux%u usage=%08x ordinary=%d\n",
            int(d.hudBoundary),(unsigned long long)vs->codeHash,(unsigned long long)ps->codeHash,int(r.depthOn),int(r.blendOn),unsigned(r.src),unsigned(r.dst),
            texture?texture->image->width:0,texture?texture->image->height:0,texture?texture->usage:0,int(ordinary));
      }
    }
  }
  d.material=classifyMaterial(d.ps->samplerMask & d.ps->cubeSamplerMask & cubes,
      bool(state.ps),bool(rs[D3DRS_SPECULARENABLE]),state.material.Specular.r,state.material.Specular.g,
      state.material.Specular.b,state.material.Power);
  if(d.ps->terrainSpecularMask){
    d.material={};
    if(terrainSpecularPass(d.ps->samplerMask,cubes,r.depthOn,r.depthWrite,r.blendOn,
                           bool(rs[D3DRS_ALPHATESTENABLE]),r.stencilOn))
      d.material=terrainSpecularMaterial();
  }
  if(state.vs && debugMode()!=DebugView::Off)d.debugProjection=extractDebugProjection(state.vsF.data());
  if(auto ps=dynamic_cast<PixelShader*>(state.ps.get());ps&&ps->codeHash==0xf65220e267055268ull&&ps->bytecode.size()==164){
    // A known water program in an unsupported pass must not fall back to the
    // generic cube hint and reconstruct the underwater terrain as its surface.
    d.material={};
    const bool accepted=worldWaterProgram(ps->codeHash,ps->bytecode.size(),d.ps->samplerMask&d.ps->cubeSamplerMask&cubes,
      r.blendOn&&r.src==Blend::One&&r.dst==Blend::InvSrcAlpha&&r.blend==BlendOp::Add&&r.depthCompare==Compare::LessEqual,
      r.depthOn,r.depthWrite,bool(rs[D3DRS_ALPHATESTENABLE]),r.stencilOn);
    if(accepted){
      d.material=worldWaterMaterial();
      d.waterBasisValid=d.debugProjection.valid&&waterViewBasis(state.vsF.data(),d.waterBasis);
      if(!d.ps->waterVariant){auto shader=std::make_shared<Shader>(*d.ps);shader->gpu.reset();shader->materialVariant.reset();shader->waterVariant.reset();
        shader->source=waterNormalSource(d.ps->source);shader->hash=shaderHash(shader->source.data(),shader->source.size());d.ps->waterVariant=shader;}
    }
    static unsigned logged=0;
    if(logged<8&&fo2MetalRaceActive()&&(debugMode()==DebugView::SSR||debugMode()==DebugView::SSRMask)&&vp.Width>512&&d.color->width==vp.Width){
      ++logged;std::printf("[FO2 WATER STATE] accepted=%d basis=%d viewport=%ux%u depth=%d/write=%d blend=%d/%d/%d alpha=%u stencil=%d\n",
        int(accepted),int(d.waterBasisValid),vp.Width,vp.Height,int(r.depthOn),int(r.depthWrite),int(r.blendOn),int(r.src),int(r.dst),unsigned(rs[D3DRS_ALPHATESTENABLE]),int(r.stencilOn));
    }
  }
  if(d.captureMaterial)d.physicalWaterAsset=fo2WaterPhysicalAsset();
  int rayOnly=metalRayOnly?metalRayOnly:fo2OffcameraRayOnlyDraw();
  d.rayOnly=rayOnly!=0;d.rayOnlyDetached=rayOnly==2;
  return d;
}
static uint32_t vertexCount(D3DPRIMITIVETYPE t, UINT n) {
  switch (t) {
  case D3DPT_POINTLIST:
    return n;
  case D3DPT_LINELIST:
    return n * 2;
  case D3DPT_LINESTRIP:
    return n + 1;
  case D3DPT_TRIANGLELIST:
    return n * 3;
  case D3DPT_TRIANGLESTRIP:
  case D3DPT_TRIANGLEFAN:
    return n + 2;
  default:
    throw std::runtime_error("Invalid primitive type");
  }
}
// Opt-in trace of lower-left HUD vertices: diagnose the original fade state
// without imposing a replacement timeout or changing game logic.
static void traceHudDraw(const State &state, const Draw &draw) {
  static const char *path = std::getenv("FO2_HUD_TRACE");
  if (!path || !*path)
    return;
  auto frame = presentCount.load();
  if (frame % 30)
    return;
  static FILE *file = std::fopen(path, "w");
  if (!file)
    return;
  static uint32_t last = UINT32_MAX, records = 0;
  if (last != frame) {
    last = frame;
    records = 0;
  }
  if (records >= 64)
    return;
  bool rhw = false;
  int position = -1, color = -1;
  for (auto input : draw.vs->inputs) {
    if ((input[1] == D3DDECLUSAGE_POSITION ||
         input[1] == D3DDECLUSAGE_POSITIONT) &&
        !input[2]) {
      position = input[0];
      rhw = input[1] == D3DDECLUSAGE_POSITIONT;
    }
    if (input[1] == D3DDECLUSAGE_COLOR && !input[2])
      color = input[0];
  }
  if (position < 0)
    return;
  if (!rhw) {
    auto w = state.vsF.data() + 12;
    if (!state.vs ||
        std::fabs(w[0]) + std::fabs(w[1]) + std::fabs(w[2]) > 1e-6f)
      return;
  }
  const Attribute *pa = nullptr, *ca = nullptr;
  for (auto &a : draw.attributes) {
    if (a.slot == unsigned(position))
      pa = &a;
    if (a.slot == unsigned(color))
      ca = &a;
  }
  if (!pa || (ca && pa->stream != ca->stream) || !draw.streams[pa->stream] ||
      (pa->type != Element::Float3 && pa->type != Element::Float4) ||
      (ca && ca->type != Element::Color && ca->type != Element::Float4))
    return;
  auto &bytes = draw.streams[pa->stream]->bytes;
  unsigned samples = 0;
  float lo = 1e9f, hi = -1e9f, px0 = 1e9f, px1 = -1e9f, py0 = 1e9f, py1 = -1e9f;
  auto &vp = state.viewport;
  for (unsigned i = 0; i < std::min(draw.count, 4096u); ++i) {
    int64_t vertex = int64_t(draw.vertexStart) + i;
    if (draw.indices) {
      uint32_t n = 0;
      auto offset = (uint64_t(draw.indexStart) + i) * draw.indexSize;
      if (offset + draw.indexSize > draw.indices->bytes.size())
        break;
      std::memcpy(&n, draw.indices->bytes.data() + offset, draw.indexSize);
      vertex = int64_t(n) + draw.baseVertex;
    }
    if (vertex < 0)
      continue;
    auto offset = uint64_t(draw.offsets[pa->stream]) +
                  uint64_t(vertex) * draw.strides[pa->stream];
    unsigned size = pa->type == Element::Float4 ? 16 : 12,
             colorSize = ca ? (ca->type == Element::Color ? 4 : 16) : 0;
    if (offset + pa->offset + size > bytes.size() ||
        (ca && offset + ca->offset + colorSize > bytes.size()))
      continue;
    float p[4] = {0, 0, 0, 1};
    std::memcpy(p, bytes.data() + offset + pa->offset, size);
    float x = p[0], y = p[1];
    if (!rhw) {
      float clip[4]{};
      for (unsigned row = 0; row < 4; ++row)
        for (unsigned col = 0; col < 4; ++col)
          clip[row] += state.vsF[row * 4 + col] * p[col];
      if (std::fabs(clip[3]) < 1e-10f)
        continue;
      x = vp.X + (clip[0] / clip[3] + 1) * 0.5f * vp.Width;
      y = vp.Y + (1 - clip[1] / clip[3]) * 0.5f * vp.Height;
    }
    if (x < vp.X || x > vp.X + vp.Width * 0.45f ||
        y < vp.Y + vp.Height * 0.7f || y > vp.Y + vp.Height + 2)
      continue;
    float alpha = 1;
    if (ca && ca->type == Element::Color)
      alpha = bytes[offset + ca->offset + 3] / 255.0f;
    else if (ca)
      std::memcpy(&alpha, bytes.data() + offset + ca->offset + 12, 4);
    lo = std::min(lo, alpha);
    hi = std::max(hi, alpha);
    px0 = std::min(px0, x);
    px1 = std::max(px1, x);
    py0 = std::min(py0, y);
    py1 = std::max(py1, y);
    ++samples;
  }
  if (!samples)
    return;
  ++records;
  auto texture = draw.textures[0];
  std::fprintf(file,
               "frame=%u vs=%llu ps=%llu roi=[%.1f %.1f %.1f %.1f] vertices=%u "
               "alpha=[%.6g %.6g] blend=%u src=%u dst=%u texture=%p/%ux%u "
               "alphaOp=%u vconst=[",
               frame, (unsigned long long)draw.vs->hash,
               (unsigned long long)draw.ps->hash, px0, py0, px1, py1, samples,
               lo, hi, unsigned(draw.raster.blendOn), unsigned(draw.raster.src),
               unsigned(draw.raster.dst), texture.get(),
               texture ? texture->width : 0, texture ? texture->height : 0,
               state.tss[0][D3DTSS_ALPHAOP]);
  for (unsigned i = 0; i < 32; ++i)
    std::fprintf(file, "%s%.6g", i ? " " : "", state.vsF[i]);
  std::fprintf(file, "] pconst=[");
  for (size_t i = 0; i < std::min<size_t>(32, draw.psConstants.size() / 4);
       ++i) {
    float value;
    std::memcpy(&value, draw.psConstants.data() + i * 4, 4);
    std::fprintf(file, "%s%.6g", i ? " " : "", value);
  }
  std::fprintf(file, "]\n");
  std::fflush(file);
  static std::set<uint64_t> saved;
  std::filesystem::path folder =
      std::filesystem::path(path).parent_path() / "hud-shaders";
  for (auto shader : {draw.vs, draw.ps})
    if (saved.insert(shader->hash).second) {
      std::filesystem::create_directories(folder);
      std::ofstream(folder / (std::to_string(shader->hash) + ".metal"))
          << shader->source;
    }
}

static void captureParticleStrip(const State &state, const uint8_t *before,
                                 const Bytes &after, size_t changed, const VertexBuffer &buffer) {
  static const char *directory = std::getenv("FO2_PARTICLE_CAPTURE");
  static unsigned saved = 0;
  if (!directory || !*directory || (!changed && saved >= 4) || saved >= 16 ||
      after.size() > (256u << 10)) return;
  std::filesystem::create_directories(directory);
  auto prefix = std::filesystem::path(directory) /
      ("strip-" + std::to_string(presentCount.load()) + "-" + std::to_string(saved++));
  for (bool repaired : {false, true}) {
    std::ofstream file(prefix.string() + (repaired ? "-after.bin" : "-before.bin"),
                       std::ios::binary);
    file.write(reinterpret_cast<const char *>(repaired ? after.data() : before),
               after.size());
  }
  std::ofstream metadata(prefix.string() + ".txt");
  metadata.precision(9);
  metadata << "buffer_size=" << buffer.data->bytes.size()
           << "\nlock_flags=" << buffer.lastLockFlags << "\ndiscards=" << buffer.discards << '\n';
  metadata << "stride=24\nvertices=" << after.size() / 24
           << "\nchanged=" << changed << "\nvs_f0_3=";
  for (unsigned i = 0; i < 16; ++i) metadata << ' ' << state.vsF[i];
  metadata << '\n';
  if (auto vs = dynamic_cast<VertexShader *>(state.vs.get())) {
    std::ofstream shader(prefix.string() + "-vs.bin", std::ios::binary);
    shader.write(reinterpret_cast<const char *>(vs->bytecode.data()), vs->bytecode.size());
  }
}
HRESULT Device::draw(D3DPRIMITIVETYPE t, UINT start, UINT n, INT base,
                     bool indexed) {
  if (!n)
    return D3D_OK;
  std::shared_ptr<Buffer> particleSnapshot;
  if (state.vs && state.strides[0] == 24) {
    auto vb = dynamic_cast<VertexBuffer *>(state.streams[0].get());
    if (vb && (vb->usage & D3DUSAGE_DYNAMIC) && vb->data->bytes.size() >= (512u << 10)) {
      ++particleCounters.candidates;
      if (indexed) ++particleCounters.indexed;
      auto c = state.vsF.data() + 12;
      if (std::fabs(c[0]) + std::fabs(c[1]) + std::fabs(c[2]) < 1e-6f)
        ++particleCounters.flat;
    }
  }
  static const bool repair = [] {
    auto e = std::getenv("FO2_PARTICLE_FIX");
    return !(e && e[0] == '0');
  }();
  if (repair && !indexed && t == D3DPT_TRIANGLESTRIP && state.vs &&
      state.strides[0] == 24) {
    auto vb = dynamic_cast<VertexBuffer *>(state.streams[0].get());
    auto c = state.vsF.data() + 12;
    if (vb && (vb->usage & D3DUSAGE_DYNAMIC) &&
        vb->data->bytes.size() >= (512u << 10) &&
        std::fabs(c[0]) + std::fabs(c[1]) + std::fabs(c[2]) >= 1e-6f) {
      size_t offset = uint64_t(state.offsets[0]) + uint64_t(start) * 24,
             bytes = uint64_t(vertexCount(t, n)) * 24;
      if (offset <= vb->data->bytes.size() &&
          bytes <= vb->data->bytes.size() - offset) {
        static const float jump = [] {
          auto e = std::getenv("FO2_PARTICLE_JUMP");
          return e ? std::atof(e) : 4.0;
        }();
        particleSnapshot = std::make_shared<Buffer>();
        particleSnapshot->transient = true;
        particleSnapshot->bytes.assign(vb->data->bytes.begin() + offset,
                                       vb->data->bytes.begin() + offset +
                                           bytes);
        ++particleCounters.strips;
        const size_t changed = repairParticleStrip(particleSnapshot->bytes.data(), bytes / 24, jump);
        if (changed) { ++particleCounters.corrected; particleCounters.vertices += changed; }
        captureParticleStrip(state, vb->data->bytes.data() + offset,
                             particleSnapshot->bytes, changed, *vb);
      }
    }
  }
  auto d = prepare();
  d.count = vertexCount(t, n);
  d.baseVertex = base;
  if (indexed) {
    if (!d.indices)
      return D3DERR_INVALIDCALL;
    d.indexStart = start;
    if ((uint64_t(start) + d.count) * d.indexSize > d.indices->bytes.size())
      return D3DERR_INVALIDCALL;
  } else {
    d.indices.reset();
    d.vertexStart = particleSnapshot ? 0 : start;
    if (particleSnapshot) {
      d.streams[0] = particleSnapshot;
      d.offsets[0] = 0;
    }
  }
  d.primitive = t == D3DPT_POINTLIST      ? Primitive::Points
                : t == D3DPT_LINELIST     ? Primitive::Lines
                : t == D3DPT_LINESTRIP    ? Primitive::LineStrip
                : t == D3DPT_TRIANGLELIST ? Primitive::Triangles
                                          : Primitive::TriangleStrip;
  if (t == D3DPT_TRIANGLEFAN) {
    auto indices = std::make_shared<Buffer>();
    indices->bytes.resize(n * 3 * 4);
    for (unsigned i = 0; i < n; ++i) {
      for (unsigned j = 0; j < 3; ++j) {
        uint32_t k = j == 0 ? 0 : i + j;
        uint32_t v = start + k;
        if (indexed) {
          if (d.indexSize == 2) {
            uint16_t z;
            std::memcpy(&z, d.indices->bytes.data() + (start + k) * 2, 2);
            v = z;
          } else
            std::memcpy(&v, d.indices->bytes.data() + (start + k) * 4, 4);
        }
        std::memcpy(indices->bytes.data() + (i * 3 + j) * 4, &v, 4);
      }
    }
    d.indices = indices;
    d.indexSize = 4;
    d.indexStart = 0;
    d.count = n * 3;
    d.primitive = Primitive::Triangles;
  }
  // D3D9 ignores CCW stencil states for points and lines.
  if (d.primitive == Primitive::Points || d.primitive == Primitive::Lines ||
      d.primitive == Primitive::LineStrip)
    d.raster.twoSidedStencil = false;
  traceHudDraw(state, d);
  captureMaterial(d);
  renderer->setDebugCapture(debugMode()!=DebugView::Off && fo2MetalRaceActive(),debugMode()==DebugView::SSR||debugMode()==DebugView::SSRMask,
      (debugMode()==DebugView::SSR||debugMode()==DebugView::SSRMask)&&debugMaterials());
  renderer->draw(d);
  return D3D_OK;
}
HRESULT Device::DrawPrimitiveUP(D3DPRIMITIVETYPE t, UINT n, const void *data,
                                UINT stride) {
  if (!data || !stride)
    return D3DERR_INVALIDCALL;
  auto vb = adopt<IDirect3DVertexBuffer9>(new VertexBuffer(
      vertexCount(t, n) * stride, 0, 0, D3DFMT_VERTEXDATA, D3DPOOL_SYSTEMMEM));
  auto b = static_cast<VertexBuffer *>(vb.get());
  std::memcpy(b->data->bytes.data(), data, b->data->bytes.size());
  state.streams[0] = vb;
  state.strides[0] = stride;
  state.offsets[0] = 0;
  auto hr = draw(t, 0, n, 0, false);
  state.streams[0].reset();
  return hr;
}
HRESULT Device::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE t, UINT min,
                                       UINT vertices, UINT n, const void *idx,
                                       D3DFORMAT fmt, const void *data,
                                       UINT stride) {
  if (!data || !idx || !stride)
    return D3DERR_INVALIDCALL;
  auto vb = adopt<IDirect3DVertexBuffer9>(new VertexBuffer(
      (min + vertices) * stride, 0, 0, D3DFMT_VERTEXDATA, D3DPOOL_SYSTEMMEM));
  auto ib = adopt<IDirect3DIndexBuffer9>(
      new IndexBuffer(vertexCount(t, n) * (fmt == D3DFMT_INDEX32 ? 4 : 2), 0, 0,
                      fmt, D3DPOOL_SYSTEMMEM));
  auto v = static_cast<VertexBuffer *>(vb.get());
  auto i = static_cast<IndexBuffer *>(ib.get());
  std::memcpy(v->data->bytes.data(), data, v->data->bytes.size());
  std::memcpy(i->data->bytes.data(), idx, i->data->bytes.size());
  state.streams[0] = vb;
  state.strides[0] = stride;
  state.offsets[0] = 0;
  state.indices = ib;
  auto hr = draw(t, 0, n, 0, true);
  state.streams[0].reset();
  state.indices.reset();
  return hr;
}
} // namespace fo2metal
uint32_t g_nfsScreenRotation = 0;
extern "C" {
unsigned long long g_nfs_call_count = 0;
IDirect3D9 *Direct3DCreate9(UINT) { return new fo2metal::Adapter(); }
void dxvkAndroidSetWindow(void *w) {
  fo2metal::hostWindow = w;
  if (w)
    fo2MetalPrepareWindow(w);
  else
    fo2MetalDestroyWindow();
}
HWND dxvkAndroidGetWindowHandle() {
  return reinterpret_cast<HWND>(fo2metal::hostWindow);
}
void dxvkAndroidSetRenderSize(uint32_t w, uint32_t h) {
  fo2metal::renderWidth = w;
  fo2metal::renderHeight = h;
}
uint32_t getD3D9PresentCount() { return fo2metal::presentCount.load(); }
static thread_local bool gameThread = false;
void d3d9NoteGameThread() { gameThread = true; }
bool nfsIsGameThread() { return gameThread; }
static std::array<std::atomic<uint64_t>, 14> waitTimes{};
void nfsCsWait(uint32_t, uint64_t ns) { waitTimes[1] += ns; }
void nfsWaitAdd(int kind, uint64_t ns) {
  if (kind >= 0 && kind < 14)
    waitTimes[kind] += ns;
}
void nfsSetPaceOnGameThread(bool) {}
void nfsEmulatePace() {}
}
bool toggleD3D9FpsCounter() {
  return fo2metal::showFps = !fo2metal::showFps.load();
}
void requestD3D9Screenshot() { fo2metal::screenshot = true; }
void backendDeferBufferCreation(bool) {}
void backendDeferTextureCreation(bool) {}
bool backendUploadDecodedLevel(IDirect3DTexture9 *texture, uint32_t level,
                               const uint8_t *rgba, const uint8_t *) {
  auto t = dynamic_cast<fo2metal::Texture2D *>(texture);
  if (!t || !rgba || level >= t->data->image->levels)
    return false;
  auto &bytes = t->data->image->rgba[level];
  std::memcpy(bytes.data(), rgba, bytes.size());
  t->data->storeRGBA(0, level, bytes);
  ++t->data->image->version;
  return true;
}
bool backendDrawIndexedInstanced(IDirect3DDevice9 *, const uint32_t *,
                                 const uint16_t *, uint32_t, uint32_t,
                                 const float *) {
  return false;
}
// Resource-side loading checks deliberately use no Renderer/Metal device.
// Exercise the same COM objects, pitch/face/mip addressing and lifetime paths
// as the cube textures immediately preceding the desktop loading crash.
int fo2CheckMetalMemoryCPU() {
  using namespace fo2metal;
  auto check = [](HRESULT hr) {
    if (FAILED(hr))
      throw std::runtime_error("CPU resource check HRESULT " +
                               std::to_string(uint32_t(hr)));
  };
  {
    const uint8_t source[] = {1, 2, 3, 99, 99, 4, 5, 6, 99, 99, 7, 8, 9};
    uint8_t destination[21];
    std::memset(destination, 0xa5, sizeof destination);
    copyUploadRows(destination, sizeof destination, 7, source, sizeof source,
                   5, 3, 3);
    for (unsigned i = 0; i < sizeof destination; ++i) {
      const uint8_t expected = i % 7 < 3 ? uint8_t(i / 7 * 3 + i % 7 + 1) : 0xa5;
      if (destination[i] != expected)
        throw std::runtime_error("Upload payload/padding boundary mismatch");
    }
    struct Invalid { uint64_t dstBytes; int64_t dstPitch;
                     uint64_t srcBytes, srcPitch; };
    for (auto bad : {Invalid{16, 7, 13, 5}, Invalid{21, 7, 12, 5},
                     Invalid{21, 2, 13, 5}, Invalid{21, -1, 13, 5},
                     Invalid{21, 7, 13, 2},
                     Invalid{21, 7, UINT64_MAX, UINT64_MAX}}) {
      std::memset(destination, 0xa5, sizeof destination);
      bool rejected = false;
      try {
        copyUploadRows(destination, bad.dstBytes, bad.dstPitch, source,
                       bad.srcBytes, bad.srcPitch, 3, 3);
      } catch (const std::runtime_error &) { rejected = true; }
      if (!rejected || !std::all_of(std::begin(destination), std::end(destination),
                                    [](uint8_t x) { return x == 0xa5; }))
        throw std::runtime_error("Invalid upload was not rejected before writing");
    }
    std::puts("[Metal check] CPU uploads: padded rows, truncated source/destination, "
              "invalid pitches and overflow rejection passed");
  }
  {
    // The index payload fits in the old arena tail but the vertex payload
    // forces growth. Reserving them separately invalidated the first pointer.
    for (unsigned initial : {32u, 64u, 128u}) {
      Bytes arena(initial, 0xa5), first(16, 0x13), second(192, 0x37);
      size_t used = initial - 16;
      unsigned allocations = 0, drains = 0;
      auto allocate = [&](size_t bytes) -> void * {
        ++allocations;
        if (used + bytes > arena.size()) {
          ++drains; used = 0;
          if (bytes > arena.size()) arena.resize(bytes * 2);
        }
        auto result = arena.data() + used; used += bytes; return result;
      };
      auto pair = copyPayloadPair(allocate, first.data(), first.size(),
                                  second.data(), second.size());
      if (allocations != 1 || drains != 1 ||
          std::memcmp(pair.first, first.data(), first.size()) ||
          std::memcmp(pair.second, second.data(), second.size()) ||
          (reinterpret_cast<uintptr_t>(pair.second) & 15))
        throw std::runtime_error("Paired command payload lost on arena growth");
      auto empty = copyPayloadPair(allocate, nullptr, 0, nullptr, 0);
      if (empty.first || empty.second || allocations != 1)
        throw std::runtime_error("Empty command payload allocated memory");
      bool rejected = false;
      try {
        copyPayloadPair(allocate, first.data(), SIZE_MAX, nullptr, 0);
      } catch (const std::runtime_error &) { rejected = true; }
      if (!rejected || allocations != 1)
        throw std::runtime_error("Command payload overflow reached allocator");
    }
    std::puts("[Metal check] CPU queue: paired index/vertex payloads survive "
              "arena rollover/growth; empty and overflow cases passed");
  }
  {
    auto data = std::make_shared<TextureData>(128, 128, 1, 1,
        D3DFMT_A8R8G8B8, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, nullptr);
    auto texture = adopt(new Texture2D(data));
    std::atomic<bool> start{false}, valid{true};
    auto queryAndRelease = [&] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      for (unsigned i = 0; i < 20000; ++i) {
        IDirect3DSurface9 *surface = nullptr;
        if (FAILED(texture->GetSurfaceLevel(0, &surface)) || !surface) {
          valid = false; return;
        }
        D3DSURFACE_DESC desc{};
        if (FAILED(surface->GetDesc(&desc)) || desc.Width != 128 ||
            desc.Height != 128) valid = false;
        surface->Release();
      }
    };
    std::thread a(queryAndRelease), b(queryAndRelease);
    start.store(true, std::memory_order_release);
    a.join(); b.join();
    if (!valid) throw std::runtime_error("Concurrent surface query/release failed");
    std::puts("[Metal check] CPU surfaces: 40000 concurrent query/description/release cycles passed");
  }
  {
    auto data = std::make_shared<TextureData>(16, 16, 1, 1,
        D3DFMT_A8R8G8B8, 0, D3DPOOL_MANAGED, nullptr);
    auto *parent = new Texture2D(data);
    IDirect3DSurface9 *child = nullptr;
    check(parent->GetSurfaceLevel(0, &child));
    parent->Release(); // The child's container is now the only native owner.
    struct Delta { IUnknown *native; int32_t refDelta; bool refQueued; };
    Delta releaseChild{child, -1, true}, acquireParent{parent, 1, true};
    std::vector<Delta *> dirty{&releaseChild, &acquireParent};
    unsigned emitted = 0;
    flushReferenceDeltas(dirty, [&](IUnknown *native, int32_t delta) {
      if (!emitted && (native != parent || delta != 1))
        throw std::runtime_error("Child release overtook parent acquisition");
      ++emitted;
      if (delta > 0) native->AddRef(); else native->Release();
    });
    auto retainedParent = adopt(parent);
    if (emitted != 2 || !dirty.empty() || releaseChild.refQueued ||
        acquireParent.refQueued || releaseChild.refDelta || acquireParent.refDelta ||
        retainedParent->GetLevelCount() != 1)
      throw std::runtime_error("Reference delta ownership/cleanup mismatch");
    std::puts("[Metal check] CPU references: parent acquisition precedes last-child release");
  }
  fo2CheckMetalBridgeResetCPU();
  unsigned levelsChecked = 0;
  for (unsigned repeat = 0; repeat < 8; ++repeat)
    for (auto format : {D3DFMT_A8R8G8B8, D3DFMT_A8, D3DFMT_R5G6B5,
                        D3DFMT_L8, D3DFMT_DXT1, D3DFMT_DXT5})
      for (unsigned levels : {1u, 0u}) {
        auto data = std::make_shared<TextureData>(128, 128, levels, 6, format,
                                                  0, D3DPOOL_MANAGED, nullptr);
        auto texture = adopt(new TextureCube(data));
        for (unsigned face = 0; face < 6; ++face)
          for (unsigned level = 0; level < data->image->levels; ++level) {
            D3DLOCKED_RECT lock{};
            check(texture->LockRect(D3DCUBEMAP_FACES(face), level, &lock,
                                    nullptr, D3DLOCK_DISCARD));
            if (unsigned(lock.Pitch) != data->pitch(level))
              throw std::runtime_error("CPU cube lock pitch mismatch");
            const uint8_t value = uint8_t(1 + repeat + face * 13 + level);
            for (unsigned row = 0; row < data->rows(level); ++row)
              std::memset(static_cast<uint8_t *>(lock.pBits) +
                              size_t(row) * lock.Pitch,
                          value, data->pitch(level));
            check(texture->UnlockRect(D3DCUBEMAP_FACES(face), level));
            const unsigned id = face * data->image->levels + level;
            if (!std::all_of(data->raw[id].begin(), data->raw[id].end(),
                             [&](uint8_t byte) { return byte == value; }))
              throw std::runtime_error("CPU cube face/mip storage mismatch");
            const auto &rgba = data->image->rgba[id];
            if (rgba.size() != size_t(data->width(level)) *
                                   data->height(level) * 4)
              throw std::runtime_error("CPU cube upload size mismatch");
            // Independent checks of alpha-only/luminance boundary pixels.
            for (size_t offset : {size_t(0), rgba.size() - 4}) {
              if (format == D3DFMT_A8 &&
                  (rgba[offset] || rgba[offset + 1] || rgba[offset + 2] ||
                   rgba[offset + 3] != value))
                throw std::runtime_error("CPU alpha cube boundary mismatch");
              if (format == D3DFMT_L8 &&
                  (rgba[offset] != value || rgba[offset + 1] != value ||
                   rgba[offset + 2] != value || rgba[offset + 3] != 255))
                throw std::runtime_error("CPU luminance cube boundary mismatch");
            }
            ++levelsChecked;
          }
        // Later face uploads must not overwrite earlier faces or mip levels.
        for (unsigned face = 0; face < 6; ++face)
          for (unsigned level = 0; level < data->image->levels; ++level) {
            const auto &raw = data->raw[face * data->image->levels + level];
            const uint8_t value = uint8_t(1 + repeat + face * 13 + level);
            if (!std::all_of(raw.begin(), raw.end(),
                             [&](uint8_t byte) { return byte == value; }))
              throw std::runtime_error("CPU cube upload crossed a face/mip boundary");
          }
        // A surface retains its container even after the caller drops it.
        IDirect3DSurface9 *surfaceRaw = nullptr, *again = nullptr;
        check(texture->GetCubeMapSurface(D3DCUBEMAP_FACE_NEGATIVE_Z, 0, &surfaceRaw));
        auto surface = adopt(surfaceRaw);
        check(texture->GetCubeMapSurface(D3DCUBEMAP_FACE_NEGATIVE_Z, 0, &again));
        if (again != surfaceRaw)
          throw std::runtime_error("CPU cube surface identity changed");
        again->Release();
        texture.reset();
        D3DSURFACE_DESC desc{};
        check(surface->GetDesc(&desc));
        if (desc.Width != 128 || desc.Format != format)
          throw std::runtime_error("CPU retained cube surface invalid");
        D3DLOCKED_RECT lock{};
        check(surface->LockRect(&lock, nullptr, D3DLOCK_READONLY));
        check(surface->UnlockRect());
        surface.reset();
      }
  for (unsigned repeat = 0; repeat < 64; ++repeat) {
    auto buffer = adopt(new VertexBuffer(512u << 10, D3DUSAGE_DYNAMIC, 0,
                                         D3DFMT_VERTEXDATA, D3DPOOL_DEFAULT));
    void *bits = nullptr;
    check(buffer->Lock(0, 0, &bits, D3DLOCK_DISCARD));
    std::memset(bits, 0xa5, buffer->data->bytes.size());
    check(buffer->Unlock());
    check(buffer->Lock(24, 96, &bits, D3DLOCK_DISCARD));
    if (!std::all_of(buffer->data->bytes.begin(), buffer->data->bytes.end(),
                     [](uint8_t value) { return value == 0; }))
      throw std::runtime_error("Partial DISCARD retained old buffer slots");
    std::memset(bits, 0x37, 24);
    check(buffer->Unlock());
    check(buffer->Lock(120, 24, &bits, D3DLOCK_NOOVERWRITE));
    std::memset(bits, 0x49, 24);
    check(buffer->Unlock());
    for (size_t i = 0; i < buffer->data->bytes.size(); ++i) {
      const uint8_t expected = i >= 24 && i < 48 ? 0x37 :
                               i >= 120 && i < 144 ? 0x49 : 0;
      if (buffer->data->bytes[i] != expected)
        throw std::runtime_error("NOOVERWRITE lost a live particle slot");
    }
    bits = reinterpret_cast<void *>(uintptr_t(1));
    if (SUCCEEDED(buffer->Lock((512u << 10) - 4, 8, &bits, 0)) ||
        bits != reinterpret_cast<void *>(uintptr_t(1)))
      throw std::runtime_error("CPU buffer accepted an out-of-bounds lock");
  }
  std::printf("[Metal check] CPU resources: %u cube face/mip uploads, "
              "surface/container lifetimes and 64 particle DISCARD cycles passed\n",
              levelsChecked);
  return 0;
}
int fo2CheckMetalAdapter() {
  using namespace fo2metal;
  auto adapter = adopt<IDirect3D9>(new Adapter());
  D3DPRESENT_PARAMETERS p{};
  p.BackBufferWidth = p.BackBufferHeight = 16;
  p.BackBufferFormat = D3DFMT_A8R8G8B8;
  p.Windowed = TRUE;
  p.EnableAutoDepthStencil = TRUE;
  p.AutoDepthStencilFormat = D3DFMT_D24S8;
  IDirect3DDevice9 *raw = nullptr;
  auto check = [](HRESULT hr) {
    if (FAILED(hr))
      throw std::runtime_error("D3D9 Metal adapter HRESULT " +
                               std::to_string(uint32_t(hr)));
  };
  check(adapter->CreateDevice(0, D3DDEVTYPE_HAL, nullptr, 0, &p, &raw));
  auto device = adopt(raw);
  // Recorded setters must leave live state unchanged; Capture and Apply must
  // restore values.
  check(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
  check(device->BeginStateBlock());
  check(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW));
  DWORD cull = 0;
  check(device->GetRenderState(D3DRS_CULLMODE, &cull));
  if (cull != D3DCULL_NONE)
    throw std::runtime_error("State recording mutated live state");
  IDirect3DStateBlock9 *sb = nullptr;
  check(device->EndStateBlock(&sb));
  auto block = adopt(sb);
  check(block->Apply());
  check(device->GetRenderState(D3DRS_CULLMODE, &cull));
  if (cull != D3DCULL_CW)
    throw std::runtime_error("State block apply failed");
  check(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
  check(block->Capture());
  check(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW));
  check(block->Apply());
  check(device->GetRenderState(D3DRS_CULLMODE, &cull));
  if (cull != D3DCULL_NONE)
    throw std::runtime_error("State block capture failed");
  IDirect3DTexture9 *tex = nullptr;
  check(device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                              &tex, nullptr));
  auto texture = adopt(tex);
  D3DLOCKED_RECT lock{};
  check(tex->LockRect(0, &lock, nullptr, 0));
  uint32_t pixels[] = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff};
  for (unsigned y = 0; y < 2; ++y)
    std::memcpy(static_cast<uint8_t *>(lock.pBits) + y * lock.Pitch,
                pixels + y * 2, 8);
  check(tex->UnlockRect(0));
  check(device->SetTexture(0, tex));
  check(device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
  check(device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
  struct Vertex {
    float x, y, z, w;
    uint32_t color;
    float u, v;
  };
  Vertex vertices[] = {{-0.5, -0.5, 0.5, 1, 0xffffffff, 0, 0},
                       {31.5, -0.5, 0.5, 1, 0xffffffff, 2, 0},
                       {-0.5, 31.5, 0.5, 1, 0xffffffff, 0, 2}};
  check(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1));
  check(device->Clear(0, nullptr,
                      D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0,
                      1, 0));
  check(
      device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, vertices, sizeof(Vertex)));
  IDirect3DSurface9 *source = nullptr;
  check(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &source));
  auto back = adopt(source);
  IDirect3DSurface9 *dest = nullptr;
  check(device->CreateOffscreenPlainSurface(16, 16, D3DFMT_A8R8G8B8,
                                            D3DPOOL_SYSTEMMEM, &dest, nullptr));
  auto staging = adopt(dest);
  check(device->GetRenderTargetData(source, dest));
  check(dest->LockRect(&lock, nullptr, D3DLOCK_READONLY));
  auto read = [&](unsigned x, unsigned y) {
    uint32_t v;
    std::memcpy(&v, static_cast<uint8_t *>(lock.pBits) + y * lock.Pitch + x * 4,
                4);
    return v;
  };
  if (read(4, 4) != 0xffff0000 || read(12, 4) != 0xff00ff00 ||
      read(4, 12) != 0xff0000ff || read(12, 12) != 0xffffffff)
    throw std::runtime_error(
        "Fixed pipeline texture orientation or BGRA readback failed");
  check(dest->UnlockRect());
  // Opposite winding must update distinct stencil faces. Verify both modes
  // through color readback, including a mode switch using the cached states.
  {
    IDirect3DStateBlock9 *savedRaw = nullptr;
    check(device->CreateStateBlock(D3DSBT_ALL, &savedRaw));
    auto saved = adopt(savedRaw);
    check(device->SetTexture(0, nullptr));
    check(device->SetRenderState(D3DRS_ZENABLE, FALSE));
    check(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
    check(device->SetRenderState(D3DRS_STENCILENABLE, TRUE));
    check(device->SetRenderState(D3DRS_STENCILMASK, 255));
    check(device->SetRenderState(D3DRS_STENCILWRITEMASK, 255));
    check(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
    check(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR));
    check(device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
    check(device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR));
    Vertex faces[] = {
        {-.5f, -.5f, .5f, 1, ~0u, 0, 0},
        {7.5f, -.5f, .5f, 1, ~0u, 0, 0},
        {-.5f, 15.5f, .5f, 1, ~0u, 0, 0},
        {7.5f, -.5f, .5f, 1, ~0u, 0, 0},
        {7.5f, 15.5f, .5f, 1, ~0u, 0, 0},
        {-.5f, 15.5f, .5f, 1, ~0u, 0, 0},
        {7.5f, -.5f, .5f, 1, ~0u, 0, 0},
        {7.5f, 15.5f, .5f, 1, ~0u, 0, 0},
        {15.5f, -.5f, .5f, 1, ~0u, 0, 0},
        {15.5f, -.5f, .5f, 1, ~0u, 0, 0},
        {7.5f, 15.5f, .5f, 1, ~0u, 0, 0},
        {15.5f, 15.5f, .5f, 1, ~0u, 0, 0}};
    for (bool twoSided : {true, false, true}) {
      check(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_STENCIL,
                          0xff000000, 1, 0));
      check(device->SetRenderState(D3DRS_COLORWRITEENABLE, 0));
      check(device->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, twoSided));
      check(device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS));
      check(device->SetRenderState(D3DRS_CCW_STENCILFUNC, D3DCMP_ALWAYS));
      check(device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE));
      check(device->SetRenderState(D3DRS_CCW_STENCILPASS, D3DSTENCILOP_INCRSAT));
      check(device->SetRenderState(D3DRS_STENCILREF, 3));
      check(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 4, faces, sizeof(Vertex)));
      check(device->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE));
      check(device->SetRenderState(D3DRS_COLORWRITEENABLE, 15));
      check(device->SetRenderState(D3DRS_STENCILWRITEMASK, 0));
      check(device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP));
      check(device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL));
      check(device->SetRenderState(D3DRS_TEXTUREFACTOR, 0xffff0000));
      check(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, vertices, sizeof(Vertex)));
      check(device->SetRenderState(D3DRS_STENCILREF, 1));
      check(device->SetRenderState(D3DRS_TEXTUREFACTOR, 0xff00ff00));
      check(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, vertices, sizeof(Vertex)));
      check(device->GetRenderTargetData(source, dest));
      check(dest->LockRect(&lock, nullptr, D3DLOCK_READONLY));
      bool correct = read(4, 8) == 0xffff0000 &&
                     read(12, 8) == (twoSided ? 0xff00ff00 : 0xffff0000);
      check(dest->UnlockRect());
      if (!correct)
        throw std::runtime_error("Clockwise/CCW stencil or mode transition failed");
      check(device->SetRenderState(D3DRS_STENCILWRITEMASK, 255));
    }
    check(saved->Apply());
    std::fprintf(stderr, "[Metal check] adapter: two-sided stencil winding and mode transitions passed\n");
  }
  // Exercise the actual PS 1.1 menu path: mutable t0 plus a dependent read.
  for (unsigned op : {0u, 69u, 70u}) {
    std::vector<DWORD> code =
        op ? std::vector<DWORD>{0xffff0101, 66,         0xb00f0000, op,
                                0xb00f0001, 0xb0e40000, 1,          0x800f0000,
                                0xb0e40001, 0x0000ffff}
           : std::vector<DWORD>{0xffff0101, 66,         0xb00f0000, 5,
                                0x800f0000, 0xb0e40000, 0x90e40000, 0x0000ffff};
    IDirect3DPixelShader9 *legacy = nullptr;
    check(device->CreatePixelShader(code.data(), &legacy));
    auto shader = adopt(legacy);
    check(device->SetPixelShader(legacy));
    check(device->SetTexture(1, tex));
    check(device->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
    check(device->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
    check(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, vertices,
                                  sizeof(Vertex)));
    check(device->GetRenderTargetData(source, dest));
    check(dest->LockRect(&lock, nullptr, D3DLOCK_READONLY));
    bool match =
        op == 69 ? (read(4, 4) == 0xffffffff && read(12, 4) == 0xff00ff00 &&
                    read(4, 12) == 0xff00ff00 && read(12, 12) == 0xffffffff)
                 : (read(4, 4) == 0xffff0000 && read(12, 4) == 0xff00ff00 &&
                    read(4, 12) == 0xff0000ff && read(12, 12) == 0xffffffff);
    check(dest->UnlockRect());
    if (!match)
      throw std::runtime_error(
          "Legacy PS 1.1 texture/dependent readback failed");
    check(device->SetPixelShader(nullptr));
  }
  // Reset may unbind a texture while a programmable shader still uses s0.
  // Keep the same render pass across the bound and null draws.
  {
    DWORD code[] = {0xffff0101, 66,         0xb00f0000, 5,
                    0x800f0000, 0xb0e40000, 0x90e40000, 0x0000ffff};
    IDirect3DPixelShader9 *rawPS = nullptr;
    check(device->CreatePixelShader(code, &rawPS));
    auto ps = adopt(rawPS);
    check(device->SetPixelShader(rawPS));
    check(device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE));
    RECT left = {0, 0, 8, 16}, right = {8, 0, 16, 16};
    check(device->SetScissorRect(&left));
    check(device->SetTexture(0, tex));
    check(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, vertices,
                                  sizeof(Vertex)));
    check(device->SetScissorRect(&right));
    check(device->SetTexture(0, nullptr));
    check(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, vertices,
                                  sizeof(Vertex)));
    check(device->GetRenderTargetData(source, dest));
    check(dest->LockRect(&lock, nullptr, D3DLOCK_READONLY));
    bool ok = read(4, 4) == 0xffff0000 && read(4, 12) == 0xff0000ff &&
              read(12, 4) == 0xff000000 && read(12, 12) == 0xff000000;
    check(dest->UnlockRect());
    if (!ok)
      throw std::runtime_error("Bound-to-null sampler transition failed");
    check(device->SetTexture(0, tex));
    check(device->SetPixelShader(nullptr));
    check(device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE));
  }
  // HUD fade: real D3DCOLOR vertices, an orthographic VS and textured PS 1.1.
  {
    DWORD vsCode[] = {
        0xfffe0101, 31,         0x80000000, 0x900f0000, 31,         0x8000000a,
        0x900f0002, 31,         0x80000005, 0x900f0003, 20,         0xc00f0000,
        0x90e40000, 0xa0e40000, 1,          0xd00f0000, 0x90e40002, 1,
        0xe0030000, 0x90540003, 0x0000ffff};
    DWORD psCode[] = {0xffff0101, 66,         0xb00f0000, 5,
                      0x800f0000, 0xb0e40000, 0x90e40000, 0x0000ffff};
    IDirect3DVertexShader9 *rawVS = nullptr;
    IDirect3DPixelShader9 *rawPS = nullptr;
    check(device->CreateVertexShader(vsCode, &rawVS));
    check(device->CreatePixelShader(psCode, &rawPS));
    auto vs = adopt(rawVS);
    auto ps = adopt(rawPS);
    D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0},
        {0, 16, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()};
    IDirect3DVertexDeclaration9 *rawDecl = nullptr;
    check(device->CreateVertexDeclaration(elements, &rawDecl));
    auto decl = adopt(rawDecl);
    check(device->SetVertexShader(rawVS));
    check(device->SetPixelShader(rawPS));
    check(device->SetVertexDeclaration(rawDecl));
    float transform[] = {0.125f, 0, 0, -1, 0, -0.125f, 0, 1,
                         0,      0, 1, 0,  0, 0,       0, 1};
    check(device->SetVertexShaderConstantF(0, transform, 4));
    check(device->SetRenderState(D3DRS_ZENABLE, FALSE));
    check(device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE));
    check(device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA));
    check(device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA));
    auto nativeDevice=dynamic_cast<Device*>(device.get());
    if(!nativeDevice||!nativeDevice->prepare().hudBoundary)throw std::runtime_error("Actual game HUD shader boundary not recognized");
    check(device->SetRenderState(D3DRS_ZENABLE, TRUE));
    if(!nativeDevice->prepare().hudBoundary)throw std::runtime_error("Depth-enabled native HUD boundary rejected");
    check(device->SetRenderState(D3DRS_ZENABLE, FALSE));
    struct HUDVertex {
      float x, y, z;
      uint32_t color;
      float u, v;
    };
    HUDVertex hud[] = {{-0.5f, -0.5f, 0.5f, 0xffffffff, 0, 0},
                       {31.5f, -0.5f, 0.5f, 0xffffffff, 2, 0},
                       {-0.5f, 31.5f, 0.5f, 0xffffffff, 0, 2}};
    for (unsigned alpha : {255u, 128u, 0u}) {
      for (auto &vertex : hud)
        vertex.color = (alpha << 24) | 0xffffff;
      check(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff0000ff, 1, 0));
      check(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, hud,
                                    sizeof(HUDVertex)));
      check(device->GetRenderTargetData(source, dest));
      check(dest->LockRect(&lock, nullptr, D3DLOCK_READONLY));
      uint32_t pixel = read(4, 4);
      check(dest->UnlockRect());
      if (std::abs(int((pixel >> 16) & 255) - int(alpha)) > 2 ||
          std::abs(int(pixel & 255) - int(255 - alpha)) > 2)
        throw std::runtime_error("HUD vertex alpha fade failed");
    }
    check(device->SetVertexShader(nullptr));
    check(device->SetPixelShader(nullptr));
    check(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1));
    check(device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
  }
  // sRGB sampling decodes to linear; sRGB render-target writes encode back.
  check(tex->LockRect(0, &lock, nullptr, 0));
  for (unsigned y = 0; y < 2; ++y)
    for (unsigned x = 0; x < 2; ++x) {
      uint32_t gray = 0xff808080;
      std::memcpy(static_cast<uint8_t *>(lock.pBits) + y * lock.Pitch + x * 4,
                  &gray, 4);
    }
  check(tex->UnlockRect(0));
  check(device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, TRUE));
  check(device->SetRenderState(D3DRS_ZENABLE, FALSE));
  auto verifyGray = [&](unsigned expected) {
    check(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, vertices,
                                  sizeof(Vertex)));
    check(device->GetRenderTargetData(source, dest));
    check(dest->LockRect(&lock, nullptr, D3DLOCK_READONLY));
    uint32_t gray = read(8, 8);
    check(dest->UnlockRect());
    for (unsigned shift : {0u, 8u, 16u})
      if (std::abs(int((gray >> shift) & 255) - int(expected)) > 2)
        throw std::runtime_error("sRGB sample/target conversion failed");
  };
  verifyGray(55);
  check(device->SetRenderState(D3DRS_SRGBWRITEENABLE, TRUE));
  verifyGray(128);
  // Release external default-pool surfaces before Reset. Managed textures
  // survive, while bindings, viewport and sRGB state return to defaults.
  back.reset();staging.reset();source=nullptr;dest=nullptr;
  for (auto size : {std::array<UINT,2>{24,20}, {16,16}, {32,24}, {16,16}}) {
    p.BackBufferWidth=size[0];p.BackBufferHeight=size[1];
    check(device->Reset(&p));
    D3DVIEWPORT9 viewport{};check(device->GetViewport(&viewport));
    DWORD srgb=1,sampleSrgb=1,fvf=1;
    check(device->GetRenderState(D3DRS_SRGBWRITEENABLE,&srgb));
    check(device->GetSamplerState(0,D3DSAMP_SRGBTEXTURE,&sampleSrgb));
    check(device->GetFVF(&fvf));
    if(viewport.X||viewport.Y||viewport.Width!=size[0]||viewport.Height!=size[1]||
       viewport.MinZ!=0||viewport.MaxZ!=1||srgb||sampleSrgb||fvf)
      throw std::runtime_error("Reset did not restore viewport/render defaults");
    IDirect3DSurface9 *rawBack=nullptr,*rawDepth=nullptr,*rawStage=nullptr;
    check(device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&rawBack));
    check(device->GetDepthStencilSurface(&rawDepth));
    auto target=adopt(rawBack),depth=adopt(rawDepth);
    D3DSURFACE_DESC colorDesc{},depthDesc{};
    check(target->GetDesc(&colorDesc));check(depth->GetDesc(&depthDesc));
    if(colorDesc.Width!=size[0]||colorDesc.Height!=size[1]||
       depthDesc.Width!=size[0]||depthDesc.Height!=size[1])
      throw std::runtime_error("Reset retained old-sized default surfaces");
    check(device->CreateOffscreenPlainSurface(size[0],size[1],D3DFMT_A8R8G8B8,
                                              D3DPOOL_SYSTEMMEM,&rawStage,nullptr));
    auto readback=adopt(rawStage);
    check(device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER|D3DCLEAR_STENCIL,
                        0xff0000ff,1,0));
    check(device->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1));
    check(device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));
    check(device->SetTexture(0,tex));
    Vertex triangle[]={{-.5f,-.5f,.5f,1,0xffffffff,0,0},
        {float(size[0]*2)-.5f,-.5f,.5f,1,0xffffffff,2,0},
        {-.5f,float(size[1]*2)-.5f,.5f,1,0xffffffff,0,2}};
    check(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST,1,triangle,sizeof(Vertex)));
    check(device->GetRenderTargetData(rawBack,rawStage));
    check(readback->LockRect(&lock,nullptr,D3DLOCK_READONLY));
    const uint32_t pixel=read(size[0]/2,size[1]/2);
    check(readback->UnlockRect());
    for(unsigned shift:{0u,8u,16u})
      if(std::abs(int((pixel>>shift)&255)-128)>2)
        throw std::runtime_error("Managed texture/drawing failed after Reset");
  }
  std::puts("[Metal check] adapter: four resizes/resets, default surfaces/state and managed-texture pixels passed");
  std::puts("[Metal check] adapter: texture lock/upload, FVF, RHW, sampling "
            "orientation, state blocks, BGRA readback and sRGB passed");
  return 0;
}
int fo2CheckMetalFixedShaders(fo2render::Renderer *renderer) {
  using namespace fo2metal;
  State state;
  state.fvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
  for (unsigned i = 0; i < 8; ++i) {
    auto &s = state.tss[i];
    s[D3DTSS_COLOROP] = D3DTOP_DISABLE;
    s[D3DTSS_TEXCOORDINDEX] = i;
    s[D3DTSS_COLORARG0] = s[D3DTSS_ALPHAARG0] = D3DTA_TFACTOR;
    s[D3DTSS_COLORARG1] = s[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
    s[D3DTSS_COLORARG2] = s[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
    s[D3DTSS_ALPHAOP] = D3DTOP_SELECTARG1;
    s[D3DTSS_RESULTARG] = D3DTA_CURRENT;
  }
  state.tss[0][D3DTSS_COLOROP] = D3DTOP_MODULATE;
  state.textures[0] =
      adopt<IDirect3DBaseTexture9>(new Texture2D(std::make_shared<TextureData>(
          2, 2, 1, 1, D3DFMT_A8R8G8B8, 0, D3DPOOL_MANAGED, nullptr)));
  std::filesystem::create_directories("work/metal/msl");
  unsigned count = 0;
  auto verify = [&](const std::shared_ptr<Shader> &shader) {
    std::string stack;
    for (char c : shader->source) {
      if (c == '(' || c == '[' || c == '{')
        stack += c;
      else if (c == ')' || c == ']' || c == '}') {
        char expected = c == ')' ? '(' : c == ']' ? '[' : '{';
        if (stack.empty() || stack.back() != expected)
          throw std::runtime_error(
              "Generated fixed MSL has unbalanced delimiters");
        stack.pop_back();
      }
    }
    if (!stack.empty())
      throw std::runtime_error("Incomplete generated MSL");
    std::ofstream("work/metal/msl/fixed-" + std::to_string(count++) + ".metal")
        << shader->source;
    if (renderer)
      renderer->validateShader(shader);
    if(!shader->vertex){
      auto variant=std::make_shared<Shader>(*shader);variant->gpu.reset();variant->source=materialOutputSource(shader->source,shader->terrainSpecularMask);
      variant->hash=shaderHash(variant->source.data(),variant->source.size());
      if(renderer)renderer->validateShader(variant);
    }
  };
  for (bool transformed : {false, true}) {
    auto elements = fvfElements(state.fvf);
    auto vs = fixedVertex(state, elements, transformed);
    verify(vs);
    for (auto op : {D3DTOP_MODULATE, D3DTOP_ADD, D3DTOP_LERP}) {
      state.tss[0][D3DTSS_COLOROP] = op;
      verify(fixedPixel(state, *vs));
    }
  }
  std::printf("[Metal check] %u generated fixed-function shaders passed%s\n",
              count, renderer ? " (GPU compilation)" : " (CPU structure)");
  return 0;
}
