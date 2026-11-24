#pragma once
#include "commands.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
namespace fo2render {
inline void validateDrawBuffers(const Draw &draw) {
  if (!draw.count || !draw.instanceCount)
    return;
  int64_t first = draw.vertexStart, last = first + draw.count - 1;
  if (draw.indices) {
    if (draw.indexSize != 2 && draw.indexSize != 4)
      throw std::runtime_error("Invalid Metal index size");
    if ((uint64_t(draw.indexStart) + draw.count) * draw.indexSize >
        draw.indices->bytes.size())
      throw std::runtime_error("Metal index buffer range exceeded");
    uint32_t minimum = UINT32_MAX, maximum = 0;
    bool found = false;
    for (uint32_t i = 0; i < draw.count; ++i) {
      uint32_t index = 0;
      std::memcpy(&index,
                  draw.indices->bytes.data() +
                      (uint64_t(draw.indexStart) + i) * draw.indexSize,
                  draw.indexSize);
      if ((draw.primitive == Primitive::TriangleStrip ||
           draw.primitive == Primitive::LineStrip) &&
          index == (draw.indexSize == 2 ? 0xffffu : 0xffffffffu))
        continue;
      minimum = std::min(minimum, index);
      maximum = std::max(maximum, index);
      found = true;
    }
    if (!found)
      return;
    first = int64_t(minimum) + draw.baseVertex;
    last = int64_t(maximum) + draw.baseVertex;
  }
  if (first < 0)
    throw std::runtime_error("Negative Metal vertex index after baseVertex");
  static const unsigned sizes[] = {4, 8, 12, 16, 4, 4, 4, 8,
                                   4, 4, 8,  4,  8, 4, 8};
  for (auto a : draw.attributes) {
    if (a.stream >= draw.streams.size() || !draw.streams[a.stream])
      throw std::runtime_error("Metal draw has an unbound vertex stream");
    auto type = unsigned(a.type);
    if (type >= sizeof(sizes) / sizeof(*sizes))
      throw std::runtime_error("Unknown Metal vertex format");
    uint64_t index = draw.strides[a.stream] ? uint64_t(last) : 0;
    if (draw.perInstanceMask & (1u << a.stream))
      index = (draw.instanceCount - 1) / std::max(1u, draw.stepRates[a.stream]);
    uint64_t end = uint64_t(draw.offsets[a.stream]) +
                   index * draw.strides[a.stream] + a.offset + sizes[type];
    if (end > draw.streams[a.stream]->bytes.size())
      throw std::runtime_error(
          "Metal vertex stream " + std::to_string(a.stream) + " needs " +
          std::to_string(end) + " bytes, buffer has " +
          std::to_string(draw.streams[a.stream]->bytes.size()));
  }
}
} // namespace fo2render
