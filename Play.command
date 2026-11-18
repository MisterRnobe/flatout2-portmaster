#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
export ANGLE_DEFAULT_PLATFORM="${ANGLE_DEFAULT_PLATFORM:-metal}"
export SDL_OPENGL_ES_DRIVER=1
export SDL_EGL_LIBRARY="$PWD/work/macos/lib/libEGL.dylib"
export SDL_OPENGL_LIBRARY="$PWD/work/macos/lib/libGLESv2.dylib"
export SDL_VIDEO_GL_DRIVER="$PWD/work/macos/lib/libGLESv2.dylib"
export NFSU2_SCREEN="${NFSU2_SCREEN:-1280x800}"
export FO2_RENDER="${FO2_RENDER:-1280x800}"
export NFSU2_SHOW_FPS="${NFSU2_SHOW_FPS:-1}"
# Keep the imported display mode consistent with the mode the runtime advertises.
python3 - <<'PYCFG'
import os, re, struct
from pathlib import Path
p = Path('gamedata/Savegame/device.cfg')
size = re.fullmatch(r'(\d+)x(\d+)', os.environ['FO2_RENDER'])
if p.exists() and size:
    w, h = map(int, size.groups())
    data = bytearray(p.read_bytes())
    if len(data) >= 132 and 320 <= w <= 8192 and 240 <= h <= 8192:
        struct.pack_into('<III', data, 120, w, h, 32)
        p.write_bytes(data)
PYCFG
./work/macos/fo2 "$PWD/gamedata" --windowed --cap60 "$@" 2>&1 | tee work/macos/play.log
