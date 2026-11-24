#!/usr/bin/env python3
"""Generate complete D3D9 interface shells; implementation lives in our adapter."""
from pathlib import Path
import re
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'runtime/sdk/include/dxvk/native/directx/d3d9.h').read_text()
names=['IDirect3D9','IDirect3DDevice9','IDirect3DSurface9','IDirect3DTexture9','IDirect3DCubeTexture9','IDirect3DVertexBuffer9','IDirect3DIndexBuffer9','IDirect3DVertexShader9','IDirect3DPixelShader9','IDirect3DVertexDeclaration9','IDirect3DStateBlock9','IDirect3DQuery9','IDirect3DSwapChain9']
out=['// Generated from the bundled LGPL D3D9 interface declarations; do not edit.','#pragma once','#include <d3d9.h>','#include <cstdio>','namespace fo2metal {','inline void unsupported(const char* method){std::fprintf(stderr,"[Metal] unsupported D3D9 call: %s\\n",method); }']
for name in names:
 body=re.search(r'DECLARE_INTERFACE_\('+name+r',\s*\w+\)\s*\{(.*?)\n\};',source,re.S).group(1)
 out.append('class '+name+'Stub : public '+name+' { public:')
 for m in re.finditer(r'STDMETHOD(?:_\(([^,]+),\s*(\w+)\)|\((\w+)\))\s*\((.*?)\)\s*PURE;',body,re.S):
  ret=m[1] or 'HRESULT';method=m[2] or m[3];args=re.sub(r'\bTHIS_?\s*','',m[4]).strip();args=' '.join(args.split())
  value='D3DERR_INVALIDCALL' if ret=='HRESULT' else '{}'
  code='unsupported("'+name+'::'+method+'");'+('return '+value+';' if ret!='void' else '')
  out.append(ret+' STDMETHODCALLTYPE '+method+'('+args+') override { '+code+' }')
 out.append('};')
out.append('}')
p=ROOT/'work/metal/include/d3d9_stubs.h';p.parent.mkdir(parents=True,exist_ok=True);p.write_text('\n'.join(out)+'\n')
