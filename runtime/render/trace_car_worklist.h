#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>
namespace fo2render {
inline constexpr uint32_t traceCarNoOwner=UINT32_MAX;
// Index by the same merged primitive ID returned by the RT intersection.
// Unknown/uncaptured materials must never borrow a neighboring car's shader.
inline std::vector<uint32_t> traceCarPrimitiveOwners(uint32_t triangles,
    const std::vector<std::array<uint32_t,2>>& ranges){
  if(!triangles||triangles>250000||ranges.size()>64)throw std::runtime_error("RT car worklist bounds");
  std::vector<uint32_t> owners(triangles,traceCarNoOwner);
  for(uint32_t material=0;material<ranges.size();++material){auto r=ranges[material];
    if(!r[1]||r[0]>=triangles||r[1]>triangles-r[0])throw std::runtime_error("RT car worklist range");
    for(uint32_t at=r[0];at<r[0]+r[1];++at){
      if(owners[at]!=traceCarNoOwner)throw std::runtime_error("RT car worklist overlapping ranges");
      owners[at]=material;
    }
  }return owners;
}
inline std::size_t traceCarWorklistBytes(std::size_t triangles,std::size_t pixels,std::size_t materials){
  if(!triangles||triangles>250000||!pixels||pixels>2048*2048||materials>64)
    throw std::runtime_error("RT car worklist allocation bounds");
  return triangles*4+pixels*4+materials*(8+16+12);
}
}
