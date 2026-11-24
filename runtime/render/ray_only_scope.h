#pragma once
// A render command carries the producer's provenance explicitly. The render
// thread must never consult the game thread's future world/camera state.
extern "C" int fo2MetalSetRayOnly(int);
namespace fo2render {
class RayOnlyScope {
  int previous;
public:
  explicit RayOnlyScope(bool value):previous(fo2MetalSetRayOnly(value?1:0)){}
  ~RayOnlyScope(){fo2MetalSetRayOnly(previous);}
  RayOnlyScope(const RayOnlyScope&)=delete;
  RayOnlyScope& operator=(const RayOnlyScope&)=delete;
};
}
