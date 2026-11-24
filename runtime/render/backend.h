#pragma once
#include <d3d9.h>
#ifdef FO2_METAL
void backendDeferBufferCreation(bool);
void backendDeferTextureCreation(bool);
bool backendUploadDecodedLevel(IDirect3DTexture9 *, uint32_t, const uint8_t *,
                               const uint8_t *);
bool backendDrawIndexedInstanced(IDirect3DDevice9 *, const uint32_t *,
                                 const uint16_t *, uint32_t, uint32_t,
                                 const float *);
#else
extern thread_local bool g_glesDeferBufferGL, g_glesDeferTextureGL;
bool glesUploadDecodedLevel(IDirect3DTexture9 *, uint32_t, const uint8_t *,
                            const uint8_t *);
bool glesDrawIndexedInstanced(IDirect3DDevice9 *, const uint32_t *,
                              const uint16_t *, uint32_t, uint32_t,
                              const float *);
inline void backendDeferBufferCreation(bool on) { g_glesDeferBufferGL = on; }
inline void backendDeferTextureCreation(bool on) { g_glesDeferTextureGL = on; }
inline bool backendUploadDecodedLevel(IDirect3DTexture9 *t, uint32_t l,
                                      const uint8_t *p, const uint8_t *q) {
  return glesUploadDecodedLevel(t, l, p, q);
}
inline bool backendDrawIndexedInstanced(IDirect3DDevice9 *d, const uint32_t *a,
                                        const uint16_t *r, uint32_t c,
                                        uint32_t n, const float *p) {
  return glesDrawIndexedInstanced(d, a, r, c, n, p);
}
#endif
