#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
namespace fo2render {
// Validate both layouts before the first write. Pitch includes padding, while
// rowBytes is the actual pixel/block payload copied on each row.
inline void copyUploadRows(void *destination, uint64_t destinationBytes,
                           int64_t destinationPitch, const void *source,
                           uint64_t sourceBytes, uint64_t sourcePitch,
                           uint32_t rowBytes, uint32_t rows) {
  if (!rows || !rowBytes)
    return;
  auto invalid = [&] {
    return std::runtime_error("D3D9 upload layout mismatch: rows=" +
        std::to_string(rows) + " rowBytes=" + std::to_string(rowBytes) +
        " src=" + std::to_string(sourcePitch) + "/" +
        std::to_string(sourceBytes) + " dst=" +
        std::to_string(destinationPitch) + "/" +
        std::to_string(destinationBytes));
  };
  if (!destination || !source || destinationPitch <= 0 ||
      sourcePitch < rowBytes || uint64_t(destinationPitch) < rowBytes)
    throw invalid();
  const auto max = uint64_t(std::numeric_limits<size_t>::max());
  auto fits = [&](uint64_t pitch, uint64_t bytes) {
    return bytes >= rowBytes && pitch <= max &&
           uint64_t(rows - 1) <= (std::min(bytes, max) - rowBytes) / pitch;
  };
  if (!fits(sourcePitch, sourceBytes) ||
      !fits(uint64_t(destinationPitch), destinationBytes))
    throw invalid();
  for (uint32_t row = 0; row < rows; ++row)
    std::memcpy(static_cast<uint8_t *>(destination) +
                    size_t(row) * size_t(destinationPitch),
                static_cast<const uint8_t *>(source) +
                    size_t(row) * size_t(sourcePitch), rowBytes);
}
} // namespace fo2render
