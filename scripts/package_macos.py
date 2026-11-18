#!/usr/bin/env python3
"""Stage a portable Apple Silicon bundle from the verified game and local dependencies."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
ROOT=Path(__file__).resolve().parents[1]
STAGE=ROOT/'work/dist/FlatOut2-macOS-arm64'
SDK=ROOT/'work/portable-sdk/lib'

def run(*args):subprocess.run(list(map(str,args)),check=True)
def copy(source,target):
    target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)
def sha(path):
    digest=hashlib.sha256()
    with path.open('rb') as f:
        while data:=f.read(8<<20):digest.update(data)
    return digest.hexdigest()
def command(name,text):
    p=STAGE/name;p.write_text(text);p.chmod(0o755)

STAGE.mkdir(parents=True,exist_ok=True)
lib=STAGE/'bin/lib';lib.mkdir(parents=True,exist_ok=True)
copy(ROOT/'work/macos/fo2',STAGE/'bin/fo2')
for name in ['libEGL.dylib','libGLESv2.dylib','libavcodec.62.dylib','libavutil.60.dylib']:
    copy(ROOT/'work/macos/lib'/name,lib/name)
copy(SDK/'libSDL2-2.0.0.dylib',lib/'libSDL2-2.0.0.dylib')
copy(SDK/'libSDL3.0.dylib',lib/'libSDL3.dylib')
run('install_name_tool','-change','/opt/homebrew/opt/sdl2-compat/lib/libSDL2-2.0.0.dylib',
    '@rpath/libSDL2-2.0.0.dylib',STAGE/'bin/fo2')
for p in lib.glob('*.dylib'):
    run('install_name_tool','-id','@rpath/'+p.name,p)
    run('codesign','--force','--sign','-',p)
run('codesign','--force','--sign','-',STAGE/'bin/fo2')
for name in ['FlatOut2.exe','d3dx9_30.dll','filesystem','data.bfs','zoom_platform.bfs']:
    target=STAGE/'gamedata'/name
    if not target.exists():run('cp','-c',ROOT/'gamedata'/name,target)
saves=STAGE/'gamedata/Savegame';saves.mkdir(parents=True,exist_ok=True)
# Device configuration contains only display/audio settings, not the player profile.
config=bytearray((ROOT/'gamedata/Savegame/device.cfg').read_bytes())
struct.pack_into('<III',config,120,1280,800,32)
(saves/'device.cfg').write_bytes(config)
(STAGE/'logs').mkdir(exist_ok=True)
command('Play.command',r'''#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
if [[ "$(uname -m)" != arm64 ]] && [[ "$(sysctl -n hw.optional.arm64 2>/dev/null || echo 0)" != 1 ]]; then
    echo "This build requires an Apple Silicon Mac."; read -r -p "Press Enter..." _answer; exit 1
fi
version=$(sw_vers -productVersion)
if (( ${version%%.*} < 13 )); then
    echo "macOS 13 or later is required."; read -r -p "Press Enter..." _answer; exit 1
fi
export ANGLE_DEFAULT_PLATFORM="${ANGLE_DEFAULT_PLATFORM:-metal}"
export SDL_OPENGL_ES_DRIVER=1
export SDL_EGL_LIBRARY="$PWD/bin/lib/libEGL.dylib"
export SDL_OPENGL_LIBRARY="$PWD/bin/lib/libGLESv2.dylib"
export SDL_VIDEO_GL_DRIVER="$PWD/bin/lib/libGLESv2.dylib"
export NFSU2_SCREEN="${NFSU2_SCREEN:-1280x800}"
export FO2_RENDER="${FO2_RENDER:-1280x800}"
export FO2_SKIP_LOGO="${FO2_SKIP_LOGO:-0}"
export NFSU2_SHOW_FPS="${NFSU2_SHOW_FPS:-1}"
export NFSU2_LANG="${NFSU2_LANG:-Russian}"
mkdir -p logs gamedata/Savegame
log="logs/${FO2_LOG_NAME:-play}.log"
if [[ -f "$log" ]]; then cp "$log" "${log%.log}.previous.log"; fi
./bin/fo2 "$PWD/gamedata" --windowed --cap60 "$@" 2>&1 | tee "$log"
''')
command('Test.command',r'''#!/bin/bash
set -uo pipefail
cd "$(dirname "$0")"
FO2_LOG_NAME=runtime-check ./Play.command --check-runtime
status=$?
printf '\nRuntime check exit code: %s\n' "$status"
read -r -p "Press Enter to close..." _answer
exit "$status"
''')
(STAGE/'ПРОЧИТАЙ.txt').write_text('''FlatOut 2 — macOS / Apple Silicon

1. Полностью распакуй ZIP в обычную папку (например, на Рабочий стол).
2. Открой Play.command двойным кликом. Homebrew, Python, Steam и CrossOver для запуска не нужны.
3. При первом запуске создай собственный профиль. Сохранений другого игрока здесь нет.

Сборка arm64, цель macOS 13 и новее; проверена на macOS 26.6.2 / M3 Pro.
По умолчанию: русский язык, окно 1280x800, ограничение 60 FPS, графика через ANGLE Metal.
F10 закрывает игру. Вход в чужое интернет-лобби через OpenSpy проверен на исходной машине.

Если macOS блокирует открытие: попробуй правый клик → «Открыть». Если этого недостаточно,
после попытки запуска открой «Системные настройки → Конфиденциальность и безопасность»
и разреши запуск этой сборки. Сборка подписана локально, без нотарификации Apple.

Для проверки без игрового окна открой Test.command. Результат сохранится в logs/runtime-check.log.
Обычный лог: logs/play.log. При проблемах передай его владельцу сборки.
Профили и настройки: gamedata/Savegame. Игровые файлы и эту папку нельзя удалять перед запуском.

Для смены языка из Terminal: NFSU2_LANG=English ./Play.command
''')
licenses=STAGE/'licenses';licenses.mkdir(exist_ok=True)
for src,name in [(ROOT/'LICENSE','port-MIT.txt'),(ROOT/'third_party/mojoshader/LICENSE.txt','MojoShader.txt'),
    (ROOT/'work/portable-sdl3-src/LICENSE.txt','SDL3.txt'),(ROOT/'work/portable-sdl2-src/LICENSE.txt','SDL2-compat.txt'),
    (ROOT/'work/ffmpeg/COPYING.LGPLv2.1','FFmpeg-LGPL-2.1.txt'),
    (Path('/Applications/Visual Studio Code.app/Contents/Resources/LICENSES.chromium.html'),'ANGLE-Chromium-notices.html')]:
    copy(src,licenses/name)
run('git','-C',ROOT/'work/ffmpeg','archive','--format=tar.gz','--output='+str(licenses/'ffmpeg-8.0.1-source.tar.gz'),'HEAD')
(licenses/'FFmpeg-build.txt').write_text('''FFmpeg n8.0.1, commit 894da5ca7d742e4429ffb2af534fcda0103ef593, unmodified source included.
Built with MACOSX_DEPLOYMENT_TARGET=13.0 and:
./configure --prefix="$PWD/output" --disable-everything --disable-autodetect --disable-programs --disable-doc --disable-network --disable-avdevice --disable-avfilter --disable-avformat --disable-swscale --disable-swresample --enable-decoder=vp3 --enable-shared --disable-static --enable-pic --disable-videotoolbox --disable-audiotoolbox
make -j4 && make install
Library IDs changed to @rpath/<name>; avcodec's avutil reference uses @loader_path/libavutil.60.dylib.
Libraries are dynamically linked and may be replaced with compatible builds.
SDL3 release-3.4.18 and sdl2-compat release-2.32.74 were built unchanged with CMake, Release, arm64, CMAKE_OSX_DEPLOYMENT_TARGET=13.0, shared libraries.
Port source: https://github.com/MisterRnobe/flatout2-portmaster/tree/macos-support
''')
for p in [STAGE/'bin/fo2',*lib.glob('*.dylib')]:
    text=subprocess.check_output(['otool','-L',str(p)],text=True)
    for line in text.splitlines():
        if not line.startswith('\t'):continue
        value=line.strip().split(' (')[0]
        if value.startswith('/') and not value.startswith(('/usr/lib/','/System/Library/')) and not value.endswith(':'):
            raise SystemExit('Nonportable dependency: '+value)
    info=subprocess.check_output(['otool','-l',str(p)],text=True)
    for version in re.findall(r'\bminos (\d+\.\d+)',info):
        if tuple(map(int,version.split('.')))>(13,0):raise SystemExit('Newer macOS dependency: '+str(p)+' '+version)
    run('codesign','--verify','--strict',p)
manifest={str(p.relative_to(STAGE)):sha(p) for p in sorted(STAGE.rglob('*')) if p.is_file() and 'logs' not in p.parts}
(STAGE/'SHA256SUMS.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n')
print('Portable bundle staged:',STAGE)
