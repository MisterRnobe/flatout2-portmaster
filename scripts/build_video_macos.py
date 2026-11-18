#!/usr/bin/env python3
"""Build an isolated FFmpeg VP3 decoder, without Homebrew codec dependencies."""
import os
from pathlib import Path
import shutil
import subprocess
ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'work/ffmpeg'
OUT=ROOT/'work/macos'
REV='894da5ca7d742e4429ffb2af534fcda0103ef593' # FFmpeg n8.0.1

def run(*args, cwd=SOURCE):
    subprocess.run(list(map(str,args)),cwd=cwd,check=True)

if not SOURCE.exists():
    run('git','clone','--depth=1','--branch','n8.0.1','https://github.com/FFmpeg/FFmpeg.git',SOURCE,cwd=ROOT)
if subprocess.check_output(['git','rev-parse','HEAD'],cwd=SOURCE,text=True).strip()!=REV:
    raise SystemExit('Unexpected FFmpeg checkout; expected n8.0.1 '+REV)
run('./configure','--prefix='+str(OUT/'video'),'--disable-everything','--disable-autodetect',
    '--disable-programs','--disable-doc','--disable-network','--disable-avdevice',
    '--disable-avfilter','--disable-avformat','--disable-swscale','--disable-swresample',
    '--enable-decoder=vp3','--enable-shared','--disable-static','--enable-pic',
    '--disable-videotoolbox','--disable-audiotoolbox')
run('make','-j'+os.environ.get('JOBS','4'))
run('make','install')
lib=OUT/'lib';lib.mkdir(parents=True,exist_ok=True)
for name in ['libavutil.60.dylib','libavcodec.62.dylib']:
    dest=lib/name
    shutil.copy2(OUT/'video/lib'/name,dest)
    run('install_name_tool','-id','@rpath/'+name,dest)
    if 'avcodec' in name:
        run('install_name_tool','-change',str(OUT/'video/lib/libavutil.60.dylib'),
            '@loader_path/libavutil.60.dylib',dest)
    run('codesign','--force','--sign','-',dest)
print('Built local VP3 decoder in',lib)
