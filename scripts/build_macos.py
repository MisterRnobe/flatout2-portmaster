#!/usr/bin/env python3
"""Build the lifted game and SDL/GLES runtime for Apple Silicon."""
import concurrent.futures, os, subprocess, sys
from pathlib import Path
root=Path(__file__).resolve().parent.parent
os.chdir(root)
out=root/'work/macos'; obj=out/'obj';obj.mkdir(parents=True,exist_ok=True)
rt=root/'runtime';sdk=rt/'sdk/include';dx=sdk/'dxvk/native';mojo=root/'third_party/mojoshader';gen=root/'work/gen'
cc=os.environ.get('CC','/opt/homebrew/opt/llvm/bin/clang'); cxx=os.environ.get('CXX','/opt/homebrew/opt/llvm/bin/clang++')
common=['-arch','arm64','-mmacosx-version-min=13.0','-O2','-DNDEBUG','-D_DARWIN_C_SOURCE','-fno-strict-aliasing','-ffp-contract=off','-include',str(rt/'host/macos_compat.h')]
mojodef=['-DSUPPORT_PROFILE_'+x+'=0' for x in ['ARB1','ARB1_NV','HLSL','METAL','D3D','BYTECODE','SPIRV','GLSPIRV']]+['-DMOJOSHADER_DEPTH_CLIPPING','-DMOJOSHADER_FLIP_RENDERTARGET']
def inc(paths):return ['-I'+str(p) for p in paths]
def defs(names):return ['-D'+s for s in names]
appdef=defs(['RECOMP_TLS=__thread','RECOMP_TLS_STRUCT','NFS_PARALLEL_GUEST','NFS_FIXED_GUEST_BASE=0x10000000000ULL','NFS_R36S','NFS_GLES','NFS_D3D9_BACKEND','NFS_SDL_AUDIO','RECOMP_STRICT_ICALL','RECOMP_X87_WINDOW','FO2_D3DX_LIFTED'])+mojodef
appinc=inc([sdk/'dxvk',rt/'host',rt/'d3d9_gles',rt/'guest',rt/'native',out/'include',sdk/'recomp32',sdk/'win32hle','/opt/homebrew/include','/opt/homebrew/include/SDL2',sdk,gen])+sum((['-isystem',str(p)] for p in [dx,dx/'windows',dx/'directx']),[])
glesinc=inc([rt/'d3d9_gles',mojo,sdk/'dxvk',dx,dx/'windows',dx/'directx','/opt/homebrew/include','/opt/homebrew/include/SDL2',sdk])
units=[]
for src in ['host/main.cpp','guest/guest_runtime.cpp','guest/d3d9_bridge.cpp','native/as2_native.c','native/fo2_video.c']:
    units.append((rt/src,appdef+appinc))
units.append((rt/'d3d9_gles/d3d9_gles.cpp',mojodef+defs(['NFS_R36S','NFS_GLES'])+glesinc))
for src in ['mojoshader.c','mojoshader_common.c','profiles/mojoshader_profile_common.c','profiles/mojoshader_profile_glsl.c']:
    units.append((mojo/src,mojodef+defs(['NFS_R36S','NFS_GLES'])+inc([mojo])))
guestflags=defs(['NFS_FIXED_GUEST_BASE=0x10000000000ULL','RECOMP_TLS=__thread','RECOMP_TLS_STRUCT','RECOMP_STRICT_ICALL','RECOMP_LOCAL_REGS','RECOMP_X87_WINDOW','RECOMP_MEM_QUAL=','NFS_X87_LOCAL','NFS_X87_LW'])+['-fno-math-errno','-fno-stack-protector','-w','-fwrapv','-fexceptions','-g0','-fvisibility=hidden','-include',str(rt/'guest/arm64_guest.h')]+inc([out/'include',sdk/'recomp32',gen])
if '--host-only' not in sys.argv:
    # D3DX is cold shader/compiler code; keep its x87 stack global rather than
    # applying local-window guesses across the recovered alias boundaries.
    units += [(p,[f for f in guestflags if f != '-DNFS_X87_LW']+['-DNFS_NATIVE_TAIL'] if p.name.startswith('d3dx_') else guestflags) for p in gen.glob('*recomp_*.c')]
def compile_unit(unit):
    src,flags=unit; dest=obj/(src.stem+'.o'); log=obj/(src.stem+'.log')
    deps=[src,Path(__file__),rt/'guest/arm64_guest.h',rt/'host/macos_compat.h',*list((out/'include').glob('*.h')),*list(gen.glob('*.h'))]
    # Host headers change often during porting; generated sources are stable between lifts.
    if src.parent==gen and dest.exists() and dest.stat().st_mtime>max(p.stat().st_mtime for p in deps): return dest
    cpp=src.suffix=='.cpp'
    with log.open('w') as f:
        result=subprocess.run([cxx if cpp else cc,*common,*(['-std=c++17'] if cpp else []),*flags,'-c',str(src),'-o',str(dest)+'.tmp'],stdout=f,stderr=f)
    if result.returncode:
        print(log.read_text()[-10000:],file=sys.stderr,flush=True)
        raise RuntimeError('compile failed: '+str(src))
    Path(str(dest)+'.tmp').replace(dest)
    print('CC '+src.name,flush=True);return dest
with concurrent.futures.ThreadPoolExecutor(max_workers=int(os.environ.get('JOBS','6'))) as pool:
    list(pool.map(compile_unit,units))
objects=list(obj.glob('*.o'))
subprocess.run([cxx,'-arch','arm64','-mmacosx-version-min=13.0',*map(str,objects),'-L/opt/homebrew/lib','-lSDL2','-L'+str(out/'lib'),'-lEGL','-lGLESv2','-Wl,-rpath,@executable_path/lib','-o',str(out/'fo2')],check=True)
subprocess.run(['codesign','--force','--sign','-',str(out/'fo2')],check=True)
print('Built '+str(out/'fo2'))
