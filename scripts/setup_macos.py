#!/usr/bin/env python3
"""Fetch pinned build dependencies and import a user's FlatOut 2 installation."""
import argparse, hashlib, os, shutil, subprocess, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
PC='35548b8d0fd8aa853da67edcf7cd6134f6b9cdc7'
NFS='50a2fe4b8037719c8ce577fbb4a471d5f2084d6b'
def run(*args): subprocess.run(list(map(str,args)),check=True,cwd=ROOT)
def dependency(name,url,commit):
    dest=ROOT/'work'/name
    if not dest.exists(): run('git','clone',url,dest)
    if subprocess.check_output(['git','-C',str(dest),'rev-parse','HEAD'],text=True).strip()!=commit:
        run('git','-C',dest,'checkout',commit)
    return dest

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--game-dir',type=Path,required=True)
    ap.add_argument('--angle-dir',type=Path,help='Folder containing ARM64 libEGL.dylib and libGLESv2.dylib')
    args=ap.parse_args()
    if sys.platform!='darwin' or os.uname().machine!='arm64':ap.error('Apple Silicon macOS is required')
    game=args.game_dir.expanduser().resolve()
    exe=next((p for p in game.iterdir() if p.name.lower()=='flatout2.exe'),None)
    if not exe or hashlib.md5(exe.read_bytes()).hexdigest()!='1f8cae2b47276ca134750ba438aa35ca':
        ap.error('Unsupported FlatOut2.exe; see README (GOG / Zoom 1.2 executable required)')
    (ROOT/'work').mkdir(exist_ok=True)
    pc=dependency('pcrecomp','https://github.com/sp00nznet/pcrecomp.git',PC)
    nfs=dependency('nfsu2','https://github.com/manuelpllull/nfsu2-recompiled-r36s.git',NFS)
    patches=[*sorted((nfs/'patches/pcrecomp').glob('*.patch')), *sorted((ROOT/'scripts/patches').glob('*.patch'))]
    marker=pc/'.fo2-patches'
    key=hashlib.sha256(b''.join(p.read_bytes() for p in patches)).hexdigest()
    if not marker.exists() or marker.read_text()!=key:
        if subprocess.check_output(['git','-C',str(pc),'diff','--name-only']):
            raise SystemExit('work/pcrecomp has modifications; use a fresh dependency checkout before setup')
        for patch in patches:run('git','-C',pc,'apply',patch)
        marker.write_text(key)
    out=ROOT/'work/macos';inc=out/'include';inc.mkdir(parents=True,exist_ok=True)
    for p in (nfs/'ports/r36s/sdk/include/recomp32').glob('*.h'):shutil.copy2(p,inc/p.name)
    candidates=[args.angle_dir] if args.angle_dir else [
        Path('/Applications/Visual Studio Code.app/Contents/Frameworks/Electron Framework.framework/Versions/A/Libraries'),
        Path('/Applications/Claude.app/Contents/Frameworks/Electron Framework.framework/Versions/A/Libraries')]
    angle=next((p for p in candidates if p and (p/'libEGL.dylib').exists() and (p/'libGLESv2.dylib').exists()),None)
    if not angle:ap.error('ANGLE libraries not found; supply --angle-dir')
    lib=out/'lib';lib.mkdir(exist_ok=True)
    for name in ['libEGL.dylib','libGLESv2.dylib']:
        dest=lib/name;shutil.copy2(angle/name,dest)
        run('install_name_tool','-id','@rpath/'+name,dest)
        run('codesign','--force','--sign','-',dest)
    lift=ROOT/'work/lift';lift.mkdir(exist_ok=True)
    shutil.copy2(exe,lift/'FlatOut2.exe')
    dll=next((p for p in game.iterdir() if p.name.lower()=='d3dx9_30.dll'),None)
    if not dll:ap.error('d3dx9_30.dll is missing')
    shutil.copy2(dll,lift/'d3dx9_30.dll')
    data=ROOT/'gamedata';data.mkdir(exist_ok=True)
    for p in game.iterdir():
        if p.suffix.lower()=='.bfs' or p.name.lower() in ('filesystem','patch','flatout2.exe','d3dx9_30.dll'):
            dest=data/p.name
            if not dest.exists():run('cp','-c',p,dest)
    saves=next((p for p in game.iterdir() if p.name.lower()=='savegame'),None)
    if saves and not (data/'Savegame').exists():shutil.copytree(saves,data/'Savegame')
    (data/'Savegame').mkdir(exist_ok=True)
    print('Ready. Run scripts/lift_macos.py with Python containing capstone and pefile, then scripts/build_macos.py.')
if __name__=='__main__':main()
