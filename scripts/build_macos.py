#!/usr/bin/env python3
"""Build the lifted game with GLES or native Metal for Apple Silicon."""
import concurrent.futures, os, subprocess, sys, shutil
from pathlib import Path
root=Path(__file__).resolve().parent.parent
os.chdir(root)
import argparse
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--renderer',choices=['gles','metal'],default='gles')
parser.add_argument('--host-only',action='store_true')
parser.add_argument('--sanitize-address',action='store_true',help='Build an isolated Metal host memory diagnostic executable')
args=parser.parse_args()
metal=args.renderer=='metal'
guestbase='0x200000000000ULL' if args.sanitize_address else '0x10000000000ULL'
guestobj=root/'work/metal/obj-asan-guest' if args.sanitize_address else root/'work/macos/obj'
if args.sanitize_address and not metal: parser.error('--sanitize-address requires --renderer metal')
if metal: subprocess.run([sys.executable,str(root/'scripts/generate_metal_interfaces.py')],check=True)
out=root/'work/macos'; obj=out/'obj';obj=(root/'work/metal/obj') if metal else obj;obj.mkdir(parents=True,exist_ok=True)
if args.sanitize_address: obj=root/'work/metal/obj-asan';obj.mkdir(parents=True,exist_ok=True)
rt=root/'runtime';sdk=rt/'sdk/include';dx=sdk/'dxvk/native';mojo=root/'third_party/mojoshader';gen=root/'work/gen'
cc=os.environ.get('CC','/opt/homebrew/opt/llvm/bin/clang'); cxx=os.environ.get('CXX','/opt/homebrew/opt/llvm/bin/clang++')
common=['-arch','arm64','-mmacosx-version-min=13.0','-O2','-DNDEBUG','-D_DARWIN_C_SOURCE','-fno-strict-aliasing','-ffp-contract=off','-include',str(rt/'host/macos_compat.h')]
if args.sanitize_address: common += ['-O1','-g','-fno-omit-frame-pointer','-fno-optimize-sibling-calls','-fsanitize=address','-DFO2_ADDRESS_SANITIZER=1']
mojodef=['-DSUPPORT_PROFILE_'+x+'=0' for x in ['ARB1','ARB1_NV','HLSL','METAL','D3D','BYTECODE','SPIRV','GLSPIRV']]+['-DMOJOSHADER_DEPTH_CLIPPING','-DMOJOSHADER_FLIP_RENDERTARGET']
def inc(paths):return ['-I'+str(p) for p in paths]
def defs(names):return ['-D'+s for s in names]
if metal: mojodef=[x for x in mojodef if x!='-DSUPPORT_PROFILE_METAL=0']+['-DSUPPORT_PROFILE_METAL=1']+['-DSUPPORT_PROFILE_'+x+'=0' for x in ['GLSL','GLSL120','GLSLES','GLSLES3']]
appdef=defs(['RECOMP_TLS=__thread','RECOMP_TLS_STRUCT','NFS_PARALLEL_GUEST','NFS_FIXED_GUEST_BASE='+guestbase,'NFS_R36S','NFS_GLES','NFS_D3D9_BACKEND','NFS_SDL_AUDIO','RECOMP_STRICT_ICALL','RECOMP_X87_WINDOW','FO2_D3DX_LIFTED'])+mojodef
appinc=inc([sdk/'dxvk',rt/'host',rt/'d3d9_gles',rt/'guest',rt/'native',out/'include',sdk/'recomp32',sdk/'win32hle','/opt/homebrew/include','/opt/homebrew/include/SDL2',sdk,gen])+sum((['-isystem',str(p)] for p in [dx,dx/'windows',dx/'directx']),[])
glesinc=inc([rt/'d3d9_gles',mojo,sdk/'dxvk',dx,dx/'windows',dx/'directx','/opt/homebrew/include','/opt/homebrew/include/SDL2',sdk])
if metal: appdef=[x for x in appdef if x!='-DNFS_GLES']+['-DFO2_METAL'];appinc+=inc([root/'work/metal/include',mojo])
units=[]
for src in ['host/main.cpp','guest/guest_runtime.cpp','guest/d3d9_bridge.cpp','native/as2_native.c','native/fo2_video.c']:
    units.append((rt/src,appdef+appinc))
if metal:
    for src in ['d3d9_metal/adapter.cpp','d3d9_metal/shaders.cpp','d3d9_metal/renderer.mm','d3d9_metal/checks.cpp']:
        units.append((rt/src,appdef+appinc+(['-fobjc-arc'] if src.endswith('.mm') else [])))
else:
    units.append((rt/'d3d9_gles/d3d9_gles.cpp',mojodef+defs(['NFS_R36S','NFS_GLES'])+glesinc))
for src in ['mojoshader.c','mojoshader_common.c','profiles/mojoshader_profile_common.c','profiles/mojoshader_profile_'+('metal' if metal else 'glsl')+'.c']:
    units.append((mojo/src,mojodef+inc([mojo])))
guestflags=defs(['NFS_FIXED_GUEST_BASE='+guestbase,'RECOMP_TLS=__thread','RECOMP_TLS_STRUCT','RECOMP_STRICT_ICALL','RECOMP_LOCAL_REGS','RECOMP_X87_WINDOW','RECOMP_MEM_QUAL=','NFS_X87_LOCAL','NFS_X87_LW'])+['-fno-math-errno','-fno-stack-protector','-w','-fwrapv','-fexceptions','-g0','-fvisibility=hidden','-include',str(rt/'guest/arm64_guest.h')]+inc([out/'include',sdk/'recomp32',gen])
if not args.host_only or args.sanitize_address:
    # D3DX is cold shader/compiler code; keep its x87 stack global rather than
    # applying local-window guesses across the recovered alias boundaries.
    units += [(p,[f for f in guestflags if f != '-DNFS_X87_LW']+['-DNFS_NATIVE_TAIL'] if p.name.startswith('d3dx_') else guestflags) for p in gen.glob('*recomp_*.c')]
def compile_unit(unit):
    src,flags=unit; unitobj=guestobj if src.parent==gen else obj;unitobj.mkdir(parents=True,exist_ok=True);dest=unitobj/(src.stem+'.o'); log=unitobj/(src.stem+'.log')
    if src.parent==gen and src.stem in ('recomp_0010','recomp_0020','recomp_0023'):
        flags=flags+['-DFO2_RT_SCENE_OBSERVER=1']
    deps=[src,Path(__file__),rt/'guest/arm64_guest.h',rt/'host/macos_compat.h',*list((out/'include').glob('*.h')),*list(gen.glob('*.h'))]
    # Host headers change often during porting; generated sources are stable between lifts.
    if src.parent==gen and dest.exists() and dest.stat().st_mtime>max(p.stat().st_mtime for p in deps): return dest
    cpp=src.suffix in ('.cpp','.mm')
    with log.open('w') as f:
        result=subprocess.run([cxx if cpp else cc,*(common if src.parent!=gen else [f for f in common if f not in ['-fsanitize=address','-DFO2_ADDRESS_SANITIZER=1']]),*(['-std=c++17'] if cpp else []),*flags,'-c',str(src),'-o',str(dest)+'.tmp'],stdout=f,stderr=f)
    if result.returncode:
        print(log.read_text()[-10000:],file=sys.stderr,flush=True)
        raise RuntimeError('compile failed: '+str(src))
    Path(str(dest)+'.tmp').replace(dest)
    print('CC '+src.name,flush=True);return dest
with concurrent.futures.ThreadPoolExecutor(max_workers=int(os.environ.get('JOBS','6'))) as pool:
    list(pool.map(compile_unit,units))
objects=list(obj.glob('*.o'))
if metal:
    guests=list(guestobj.glob('*recomp_*.o'))
    if not guests: raise RuntimeError('Build the GLES guest objects first')
    objects+=guests
binary=root/'work/metal/fo2-metal' if metal else out/'fo2'
if args.sanitize_address: binary=root/'work/metal/fo2-metal-asan'
sdl=root/'work/portable-sdk/lib/libSDL2-2.0.0.dylib'
sdlflags=[str(sdl),'-Wl,-rpath,'+str(sdl.parent)] if metal and sdl.exists() else ['-L/opt/homebrew/lib','-lSDL2']
libs=['-framework','Metal','-framework','Foundation','-framework','QuartzCore'] if metal else ['-L'+str(out/'lib'),'-lEGL','-lGLESv2']
subprocess.run([cxx,'-arch','arm64','-mmacosx-version-min=13.0',*(['-fsanitize=address'] if args.sanitize_address else []),*map(str,objects),*sdlflags,*libs,'-Wl,-rpath,@executable_path/lib','-Wl,-rpath,'+str(out/'lib'),'-o',str(binary)],check=True)
if metal:
    (binary.parent/'lib').mkdir(exist_ok=True)
    for name in ['libavcodec.62.dylib','libavutil.60.dylib']:
        source=out/'lib'/name
        if source.exists():shutil.copy2(source,binary.parent/'lib'/name)
subprocess.run(['codesign','--force','--sign','-',str(binary)],check=True)
print('Built '+str(binary))
