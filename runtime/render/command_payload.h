#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
namespace fo2render {
// A command's pointers must be reserved together: a second arena allocation
// may drain/reuse or grow storage before the command has even been queued.
template<class Allocate>
std::pair<const void *, const void *> copyPayloadPair(
    Allocate allocate, const void *first, size_t firstBytes,
    const void *second, size_t secondBytes) {
  if (!first) firstBytes = 0;
  if (!second) secondBytes = 0;
  const auto max = std::numeric_limits<size_t>::max();
  if (firstBytes > max - 15)
    throw std::runtime_error("Command payload alignment overflow");
  const size_t secondOffset = (firstBytes + 15) & ~size_t(15);
  if (secondBytes > max - secondOffset)
    throw std::runtime_error("Command payload size overflow");
  const size_t bytes = secondOffset + secondBytes;
  if (!bytes) return {nullptr, nullptr};
  auto data = static_cast<uint8_t *>(allocate(bytes));
  if (firstBytes) std::memcpy(data, first, firstBytes);
  if (secondBytes) std::memcpy(data + secondOffset, second, secondBytes);
  return {firstBytes ? data : nullptr,
          secondBytes ? data + secondOffset : nullptr};
}
} // namespace fo2render
