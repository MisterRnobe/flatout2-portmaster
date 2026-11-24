#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace fo2render {
// Compatibility repair used by the existing GLES backend for incomplete FO2
// particle slots. Preserve legitimate doubled strip connectors between
// emitters.
inline size_t repairParticleStrip(uint8_t *vertices, size_t count,
                                  float jump = 4) {
  constexpr size_t stride = 24;
  static const uint8_t zero[stride]{};
  auto validPosition = [&](size_t i) {
    float p[3];
    std::memcpy(p, vertices + i * stride, sizeof p);
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]) &&
           (p[0] != 0 || p[1] != 0 || p[2] != 0);
  };
  size_t first = 0, changed = 0;
  while (first < count && !validPosition(first)) ++first;
  if (first == count) {
    for (size_t i = 0; i < count; ++i)
      if (std::memcmp(vertices + i * stride, zero, stride)) ++changed;
    if (changed) std::memset(vertices, 0, count * stride);
    return changed;
  }
  for (size_t i = 0; i < count; ++i)
    if (!validPosition(i)) {
      std::memcpy(vertices + i * stride,
                  vertices + (i < first ? first : i - 1) * stride, stride);
      ++changed;
    }
  if (jump > 0)
    for (size_t i = first + 1; i < count; ++i) {
      float a[3], b[3];
      std::memcpy(a, vertices + (i - 1) * stride, 12);
      std::memcpy(b, vertices + i * stride, 12);
      float d2 = 0;
      for (unsigned c = 0; c < 3; ++c)
        d2 += (a[c] - b[c]) * (a[c] - b[c]);
      if (d2 <= jump * jump)
        continue;
      // A,A,B,B disconnects two ribbons. B,B alone still leaves the
      // preceding X,A,B triangle spanning the jump, so it is not sufficient.
      if (i >= 2 && i + 1 < count &&
          !std::memcmp(vertices + (i - 2) * stride, vertices + (i - 1) * stride, 12) &&
          !std::memcmp(vertices + i * stride, vertices + (i + 1) * stride, 12))
        continue;
      std::memcpy(vertices + i * stride, vertices + (i - 1) * stride, stride);
      ++changed;
    }
  return changed;
}
} // namespace fo2render
