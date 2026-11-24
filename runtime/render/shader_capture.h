#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
namespace fo2render {
inline uint64_t shaderHash(const void *data, size_t length) {
  uint64_t h = 14695981039346656037ull;
  for (size_t i = 0; i < length; ++i)
    h = (h ^ static_cast<const uint8_t *>(data)[i]) * 1099511628211ull;
  return h;
}
inline void captureShader(const void *bytes, size_t length) {
  const char *directory = std::getenv("FO2_SHADER_CAPTURE");
  if (!directory || !*directory)
    return;
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  char name[40];
  std::snprintf(name, sizeof name, "%016llx.bin",
                static_cast<unsigned long long>(shaderHash(bytes, length)));
  auto path = std::filesystem::path(directory) / name;
  if (std::filesystem::exists(path))
    return;
  std::ofstream f(path, std::ios::binary);
  f.write(static_cast<const char *>(bytes), length);
}
} // namespace fo2render
