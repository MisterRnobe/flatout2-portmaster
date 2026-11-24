#pragma once
#include <d3d9.h>
#include <stdexcept>
#include <string>
namespace fo2metal {
struct FFPStage {
  DWORD colorOp, colorArg0, colorArg1, colorArg2;
  DWORD alphaOp, alphaArg0, alphaArg1, alphaArg2;
  DWORD resultArg;
  DWORD texCoordIndex;
  DWORD transformFlags;
  int texType; // 0 none, 1 2D, 2 cube
};

inline std::string ffpArg(DWORD a, bool alpha) {
  std::string b;
  switch (a & D3DTA_SELECTMASK) {
  case D3DTA_DIFFUSE:
    b = "v_diffuse";
    break;
  case D3DTA_CURRENT:
    b = "cur";
    break;
  case D3DTA_TEXTURE:
    b = "tex";
    break;
  case D3DTA_TFACTOR:
    b = "u_tfactor";
    break;
  case D3DTA_SPECULAR:
    b = "v_specular";
    break;
  case D3DTA_TEMP:
    b = "tmp";
    break;
  case D3DTA_CONSTANT:
    b = "stage_constant";
    break;
  default:
    b = "cur";
    break;
  }
  if (a & D3DTA_ALPHAREPLICATE)
    b = "float4(" + b + ".a)";
  if (a & D3DTA_COMPLEMENT)
    b = "(float4(1.0) - " + b + ")";
  return b + (alpha ? ".a" : ".rgb");
}

// Returns the MSL expression for a texture-stage operation (float3 for colour,
// float for alpha).
inline std::string ffpOp(DWORD op, const FFPStage &st, bool alpha) {
  const DWORD a0 = alpha ? st.alphaArg0 : st.colorArg0;
  const DWORD a1 = alpha ? st.alphaArg1 : st.colorArg1;
  const DWORD a2 = alpha ? st.alphaArg2 : st.colorArg2;
  const std::string x0 = ffpArg(a0, alpha), x1 = ffpArg(a1, alpha),
                    x2 = ffpArg(a2, alpha);
  const std::string x1a = ffpArg(a1, true);
  const std::string one = alpha ? "1.0" : "float3(1.0)";
  switch (op) {
  case D3DTOP_SELECTARG1:
    return x1;
  case D3DTOP_SELECTARG2:
    return x2;
  case D3DTOP_MODULATE:
    return x1 + " * " + x2;
  case D3DTOP_MODULATE2X:
    return "(" + x1 + " * " + x2 + ") * 2.0";
  case D3DTOP_MODULATE4X:
    return "(" + x1 + " * " + x2 + ") * 4.0";
  case D3DTOP_ADD:
    return x1 + " + " + x2;
  case D3DTOP_ADDSIGNED:
    return x1 + " + " + x2 + " - 0.5";
  case D3DTOP_ADDSIGNED2X:
    return "(" + x1 + " + " + x2 + " - 0.5) * 2.0";
  case D3DTOP_SUBTRACT:
    return x1 + " - " + x2;
  case D3DTOP_ADDSMOOTH:
    return x1 + " + " + x2 + " - " + x1 + " * " + x2;
  case D3DTOP_BLENDDIFFUSEALPHA:
    return "mix(" + x2 + ", " + x1 + ", v_diffuse.a)";
  case D3DTOP_BLENDTEXTUREALPHA:
    return "mix(" + x2 + ", " + x1 + ", tex.a)";
  case D3DTOP_BLENDFACTORALPHA:
    return "mix(" + x2 + ", " + x1 + ", u_tfactor.a)";
  case D3DTOP_BLENDTEXTUREALPHAPM:
    return x1 + " + " + x2 + " * (1.0 - tex.a)";
  case D3DTOP_BLENDCURRENTALPHA:
    return "mix(" + x2 + ", " + x1 + ", cur.a)";
  case D3DTOP_PREMODULATE:
    throw std::runtime_error("PREMODULATE is unsupported");
  case D3DTOP_MODULATEALPHA_ADDCOLOR:
    return alpha ? x1 : x1 + " + " + x1a + " * " + x2;
  case D3DTOP_MODULATECOLOR_ADDALPHA:
    return alpha ? x1 : x1 + " * " + x2 + " + " + x1a;
  case D3DTOP_MODULATEINVALPHA_ADDCOLOR:
    return alpha ? x1 : x1 + " + (1.0 - " + x1a + ") * " + x2;
  case D3DTOP_MODULATEINVCOLOR_ADDALPHA:
    return alpha ? x1 : "(" + one + " - " + x1 + ") * " + x2 + " + " + x1a;
  case D3DTOP_DOTPRODUCT3: {
    const std::string c1 = ffpArg(a1, false), c2 = ffpArg(a2, false);
    std::string d =
        "clamp(dot(" + c1 + " - 0.5, " + c2 + " - 0.5) * 4.0, 0.0, 1.0)";
    return alpha ? d : "float3(" + d + ")";
  }
  case D3DTOP_MULTIPLYADD:
    return x0 + " + " + x1 + " * " + x2;
  case D3DTOP_LERP:
    return "mix(" + x2 + ", " + x1 + ", " + x0 + ")";
  default:
    throw std::runtime_error("Unsupported fixed texture operation");
  }
}

} // namespace fo2metal
