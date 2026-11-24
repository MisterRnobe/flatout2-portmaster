#pragma once
#include <cstdint>
#include <cstring>
#include <d3d9.h>
namespace fo2metal {
inline UINT d3dFormatBytesPerPixel(D3DFORMAT f) {
  switch (static_cast<DWORD>(f)) {
  case D3DFMT_R5G6B5:
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A1R5G5B5:
  case D3DFMT_A4R4G4B4:
  case D3DFMT_X4R4G4B4:
  case D3DFMT_A8L8:
  case D3DFMT_D16:
    return 2;
  case D3DFMT_A8:
  case D3DFMT_L8:
  case D3DFMT_P8:
    return 1;
  default:
    return 4;
  }
}

// Converts one row of a D3D format to RGBA8 (GL_RGBA/GL_UNSIGNED_BYTE byte
// order).
inline void convertRowToRGBA8(D3DFORMAT f, const uint8_t *src, uint8_t *dst,
                              UINT width) {
  switch (static_cast<DWORD>(f)) {
  case D3DFMT_A8R8G8B8:
    for (UINT x = 0; x < width; ++x, src += 4, dst += 4) {
      dst[0] = src[2];
      dst[1] = src[1];
      dst[2] = src[0];
      dst[3] = src[3];
    }
    break;
  case D3DFMT_X8R8G8B8:
    for (UINT x = 0; x < width; ++x, src += 4, dst += 4) {
      dst[0] = src[2];
      dst[1] = src[1];
      dst[2] = src[0];
      dst[3] = 0xFF;
    }
    break;
  case D3DFMT_X8B8G8R8:
    for (UINT x = 0; x < width; ++x, src += 4, dst += 4) {
      dst[0] = src[0];
      dst[1] = src[1];
      dst[2] = src[2];
      dst[3] = 0xFF;
    }
    break;
  case D3DFMT_R5G6B5:
    for (UINT x = 0; x < width; ++x, src += 2, dst += 4) {
      uint16_t p = static_cast<uint16_t>(src[0] | (src[1] << 8));
      dst[0] = static_cast<uint8_t>(((p >> 11) & 31) * 255 / 31);
      dst[1] = static_cast<uint8_t>(((p >> 5) & 63) * 255 / 63);
      dst[2] = static_cast<uint8_t>((p & 31) * 255 / 31);
      dst[3] = 0xFF;
    }
    break;
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A1R5G5B5:
    for (UINT x = 0; x < width; ++x, src += 2, dst += 4) {
      uint16_t p = static_cast<uint16_t>(src[0] | (src[1] << 8));
      dst[0] = static_cast<uint8_t>(((p >> 10) & 31) * 255 / 31);
      dst[1] = static_cast<uint8_t>(((p >> 5) & 31) * 255 / 31);
      dst[2] = static_cast<uint8_t>((p & 31) * 255 / 31);
      dst[3] = (f == D3DFMT_X1R5G5B5 || (p & 0x8000)) ? 0xFF : 0x00;
    }
    break;
  case D3DFMT_A4R4G4B4:
  case D3DFMT_X4R4G4B4:
    for (UINT x = 0; x < width; ++x, src += 2, dst += 4) {
      uint16_t p = static_cast<uint16_t>(src[0] | (src[1] << 8));
      dst[0] = static_cast<uint8_t>(((p >> 8) & 15) * 17);
      dst[1] = static_cast<uint8_t>(((p >> 4) & 15) * 17);
      dst[2] = static_cast<uint8_t>((p & 15) * 17);
      dst[3] = (f == D3DFMT_X4R4G4B4)
                   ? 0xFF
                   : static_cast<uint8_t>(((p >> 12) & 15) * 17);
    }
    break;
  case D3DFMT_A8:
    for (UINT x = 0; x < width; ++x, ++src, dst += 4) {
      dst[0] = dst[1] = dst[2] = 0;
      dst[3] = src[0];
    }
    break;
  case D3DFMT_L8:
    for (UINT x = 0; x < width; ++x, ++src, dst += 4) {
      dst[0] = dst[1] = dst[2] = src[0];
      dst[3] = 0xFF;
    }
    break;
  case D3DFMT_A8L8:
    for (UINT x = 0; x < width; ++x, src += 2, dst += 4) {
      dst[0] = dst[1] = dst[2] = src[0];
      dst[3] = src[1];
    }
    break;
  default: // A8B8G8R8 and anything 32-bit RGBA-ordered
    std::memcpy(dst, src, width * 4);
    break;
  }
}

} // namespace fo2metal
