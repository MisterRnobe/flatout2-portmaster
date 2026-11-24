#!/usr/bin/env python3
"""Build a self-contained native Metal .app for Apple Silicon, with F8 SSR."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ['FlatOut2.exe', 'd3dx9_30.dll', 'filesystem', 'data.bfs', 'zoom_platform.bfs']


def run(*args, **kwargs):
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        while data := source.read(8 << 20):
            digest.update(data)
    return digest.hexdigest()


def copy(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)


def audit(binary, frameworks):
    arch = subprocess.check_output(['lipo', '-archs', str(binary)], text=True).strip()
    if arch != 'arm64':
        raise RuntimeError(f'Unexpected architecture {arch}: {binary}')
    commands = subprocess.check_output(['otool', '-l', str(binary)], text=True)
    for version in re.findall(r'\bminos (\d+(?:\.\d+){0,2})', commands):
        if tuple(map(int, version.split('.'))) > (13, 0, 0):
            raise RuntimeError(f'Newer macOS target {version}: {binary}')
    for path in re.findall(r'\bpath (.+?) \(offset', commands):
        if path.startswith('/'):
            raise RuntimeError(f'Absolute development rpath: {path}')
    dependencies = subprocess.check_output(['otool', '-L', str(binary)], text=True)
    for line in dependencies.splitlines():
        if not line.startswith('\t'):
            continue
        path = line.strip().split(' (')[0]
        if path.startswith(('/usr/lib/', '/System/Library/')):
            continue
        if path.startswith(('@rpath/', '@loader_path/')) and (frameworks/Path(path).name).is_file():
            continue
        raise RuntimeError(f'Unbundled dependency {path}: {binary}')
    run('codesign', '--verify', '--strict', binary)


def package(output):
    if output.exists():
        raise RuntimeError(f'Output already exists; choose another --output: {output}')
    contents = output/'Contents'
    macos, frameworks, resources = contents/'MacOS', contents/'Frameworks', contents/'Resources'
    for folder in [macos, frameworks, resources/'gamedata', resources/'defaults']:
        folder.mkdir(parents=True, exist_ok=True)
    engine = macos/'fo2'
    copy(ROOT/'work/metal/fo2-metal', engine)
    for source, name in [(ROOT/'work/portable-sdk/lib/libSDL2-2.0.0.dylib', 'libSDL2-2.0.0.dylib'),
                         (ROOT/'work/portable-sdk/lib/libSDL3.0.dylib', 'libSDL3.dylib'),
                         (ROOT/'work/macos/lib/libavcodec.62.dylib', 'libavcodec.62.dylib'),
                         (ROOT/'work/macos/lib/libavutil.60.dylib', 'libavutil.60.dylib')]:
        copy(source, frameworks/name)
    commands = subprocess.check_output(['otool', '-l', str(engine)], text=True)
    for path in re.findall(r'cmd LC_RPATH\s+cmdsize \d+\s+path (.+?) \(offset', commands):
        run('install_name_tool', '-delete_rpath', path, engine)
    run('install_name_tool', '-add_rpath', '@executable_path/../Frameworks', engine)
    for library in frameworks.glob('*.dylib'):
        run('install_name_tool', '-id', '@rpath/'+library.name, library)
        run('codesign', '--force', '--sign', '-', library)
    run('codesign', '--force', '--sign', '-', engine)
    launcher = macos/'FlatOut2'
    run(os.environ.get('CXX', '/opt/homebrew/opt/llvm/bin/clang++'), '-std=c++17', '-arch', 'arm64',
        '-mmacosx-version-min=13.0', '-O2', '-fobjc-arc', ROOT/'runtime/host/macos_app_launcher.mm',
        '-framework', 'AppKit', '-o', launcher)
    plist = dict(CFBundleExecutable='FlatOut2', CFBundleName='FlatOut 2', CFBundleDisplayName='FlatOut 2',
                 CFBundleIdentifier='io.github.MisterRnobe.flatout2.metal', CFBundleVersion='0.3.1',
                 CFBundleShortVersionString='0.3.1', CFBundlePackageType='APPL',
                 LSMinimumSystemVersion='13.0', LSArchitecturePriority=['arm64'],
                 NSHighResolutionCapable=True, NSPrincipalClass='NSApplication',
                 LSApplicationCategoryType='public.app-category.racing-games',
                 NSLocalNetworkUsageDescription='Поиск игровых комнат и подключение к игрокам в локальной сети.')
    (contents/'Info.plist').write_bytes(plistlib.dumps(plist))
    (contents/'PkgInfo').write_bytes(b'APPL????')
    for name in ASSETS:
        run('cp', '-c', ROOT/'gamedata'/name, resources/'gamedata'/name)
    config = bytearray((ROOT/'gamedata/Savegame/device.cfg').read_bytes())
    if len(config) < 132:
        raise RuntimeError('Truncated device configuration')
    struct.pack_into('<III', config, 120, 1280, 800, 32)
    (resources/'defaults/device.cfg').write_bytes(config)
    licenses = resources/'licenses'
    for source, name in [(ROOT/'LICENSE', 'port-MIT.txt'),
                         (ROOT/'third_party/mojoshader/LICENSE.txt', 'MojoShader.txt'),
                         (ROOT/'work/portable-sdl3-src/LICENSE.txt', 'SDL3.txt'),
                         (ROOT/'work/portable-sdl2-src/LICENSE.txt', 'SDL2-compat.txt'),
                         (ROOT/'work/ffmpeg/COPYING.LGPLv2.1', 'FFmpeg-LGPL-2.1.txt'),
                         (ROOT/'scripts/build_video_macos.py', 'build_video_macos.py')]:
        copy(source, licenses/name)
    run('git', '-C', ROOT/'work/ffmpeg', 'archive', '--format=tar.gz',
        '--output='+str(licenses/'ffmpeg-source.tar.gz'), 'HEAD')
    (licenses/'FFmpeg-build.txt').write_text('FFmpeg unmodified source included; VP3 decoder only, shared libraries.\n'
        'Build flags and library relocation: see build_video_macos.py. Compatible dylibs may be replaced in Contents/Frameworks.\n'
        'SDL3 release-3.4.18 and sdl2-compat release-2.32.74: unchanged shared arm64 builds, target macOS 13.\n')
    readme = '''FlatOut 2 — Apple Silicon / Metal

Распакуй архив и перенеси «FlatOut 2.app» в «Программы» или обычную папку. Открой приложение двойным кликом.
Homebrew, Python, Steam и CrossOver для запуска не нужны. Требуется Mac с Apple Silicon и macOS 13 или новее.
Сборка подписана локально; нотарификации Apple нет.

SSR включён по умолчанию. F8 переключает отражения во время игры; на мультимедийной клавиатуре используй Fn+F8.
Состояние SSR видно рядом с FPS. Переключение не сохраняет диагностические снимки.
Фоновые эксперименты с ray tracing отключены. Текущие игровые отражения используют SSR.
По умолчанию: русский язык, окно 1280x800, лимит 60 FPS. Трассу и режим выбирай в меню.

При первом запуске подготавливаются игровые данные в пользовательской папке (на APFS — быстрая копия).
Затем создай свой профиль. Чужих сохранений в приложении нет.
Сохранения: ~/Library/Application Support/FlatOut2Metal/gamedata/Savegame
Логи: ~/Library/Application Support/FlatOut2Metal/logs
В Finder можно открыть эти папки через «Переход → Переход к папке…».
Замена или перенос .app не удаляет сохранения. Для переноса профиля из старой portable-сборки
скопируй её файлы из gamedata/Savegame в указанную папку при закрытой игре.
Для обновления используй тот же путь к приложению; игровые данные останутся внутри .app.

Если macOS блокирует запуск, разрешение для этой локально подписанной сборки находится в
«Системные настройки → Конфиденциальность и безопасность» после попытки открытия.
Инструкция Apple по открытию локально подписанных приложений: https://support.apple.com/en-us/102445
При проблемах передай последний play.log из папки logs. Первое появление трассы может сопровождаться подгрузкой материалов.
'''
    (resources/'ПРОЧИТАЙ.txt').write_text(readme)
    (output.parent/'README-FlatOut2-RU.txt').write_text(readme)
    build = dict(created_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(), renderer='native-metal',
                 architecture='arm64', minimum_macos='13.0', ssr_default=True, ssr_toggle='F8',
                 ssr_capture=False, gameplay_rt=False, rt_diagnostics=False, player_saves_included=False,
                 engine_sha256=sha(engine))
    (resources/'build-info.json').write_text(json.dumps(build, indent=2)+'\n')
    manifest = {str(p.relative_to(resources)): sha(p) for p in resources.rglob('*') if p.is_file() and p.name != 'SHA256SUMS.json'}
    (resources/'SHA256SUMS.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+'\n')
    run('codesign', '--force', '--sign', '-', output)
    for binary in [engine, launcher, *frameworks.glob('*.dylib')]:
        audit(binary, frameworks)
    run('codesign', '--verify', '--deep', '--strict', output)
    print('App ready:', output, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'work/dist/FlatOut 2.app')
    parser.add_argument('--archive', action='store_true')
    args = parser.parse_args()
    output = args.output.resolve()
    if output.suffix != '.app':
        parser.error('--output must end with .app')
    package(output)
    if args.archive:
        archive = output.parent/'FlatOut2-Metal-macOS-arm64.app.zip'
        run('ditto', '-c', '-k', '--zlibCompressionLevel', '1', '--sequesterRsrc', '--keepParent', output, archive)
        run('zip', '-j', archive, output.parent/'README-FlatOut2-RU.txt')
        (archive.with_suffix(archive.suffix+'.sha256')).write_text(sha(archive)+'  '+archive.name+'\n')
        print('Archive ready:', archive, flush=True)


if __name__ == '__main__':
    main()
