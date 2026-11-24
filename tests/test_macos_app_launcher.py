import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ['FlatOut2.exe', 'd3dx9_30.dll', 'filesystem', 'data.bfs', 'zoom_platform.bfs']


class MacAppLauncherTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='fo2-app-tests-')
        cls.root = Path(cls.temp.name)
        cls.launcher = cls.root/'launcher'
        subprocess.run([os.environ.get('CXX', '/opt/homebrew/opt/llvm/bin/clang++'), '-std=c++17',
                        '-arch', 'arm64', '-mmacosx-version-min=13.0', '-fobjc-arc',
                        str(ROOT/'runtime/host/macos_app_launcher.mm'), '-framework', 'AppKit',
                        '-o', str(cls.launcher)], check=True, capture_output=True)
        source = cls.root/'engine.cpp'
        source.write_text('''#include <cstdio>
#include <cstdlib>
int main(int argc,char** argv){
  const char* labels[]={"view","ssr_materials","ssr_capture","rt","scenario","auto","shaders"};
  const char* names[]={"FO2_DEBUG_VIEW","FO2_SSR_MATERIALS","FO2_SSR_CAPTURE","FO2_RT_LIVE_DIAGNOSTIC","FO2_TEST_SCENARIO","FO2_AUTODRIVE","FO2_SHADER_CAPTURE"};
  for(unsigned i=0;i<7;++i){auto v=std::getenv(names[i]);std::printf("%s=%s\\n",labels[i],v?v:"unset");}
  std::printf("args=");for(int i=1;i<argc;++i)std::printf("%s%s",i>1?" ":"",argv[i]);std::puts("");return 0;
}
''')
        cls.engine = cls.root/'engine'
        subprocess.run([os.environ.get('CXX', '/opt/homebrew/opt/llvm/bin/clang++'), '-arch', 'arm64',
                        '-mmacosx-version-min=13.0', str(source), '-o', str(cls.engine)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def make_app(self, root):
        app = root/'Folder with spaces/FlatOut 2.app'
        macos, resources = app/'Contents/MacOS', app/'Contents/Resources'
        macos.mkdir(parents=True)
        (resources/'gamedata').mkdir(parents=True)
        (resources/'defaults').mkdir()
        (resources/'defaults/device.cfg').write_bytes(b'device-seed')
        for name in ASSETS:
            (resources/'gamedata'/name).write_text('fixture '+name)
        shutil.copy2(self.launcher, macos/'FlatOut2')
        engine = macos/'fo2'
        shutil.copy2(self.engine, engine)
        subprocess.run(['codesign', '--force', '--sign', '-', str(engine)], check=True, capture_output=True)
        (app/'Contents/Info.plist').write_bytes(plistlib.dumps(dict(
            CFBundleExecutable='FlatOut2', CFBundleIdentifier='io.github.flatout2.launcher.test', CFBundlePackageType='APPL')))
        signed = subprocess.run(['codesign', '--force', '--sign', '-', str(app)], capture_output=True, text=True)
        self.assertEqual(signed.returncode, 0, signed.stderr)
        return app

    def launch(self, app, data, *args):
        env = dict(os.environ, FO2_USER_DATA_DIR=str(data), FO2_APP_HEADLESS='1',
                   FO2_TEST_SCENARIO='auto-race', FO2_AUTODRIVE='1', FO2_SHADER_CAPTURE='/tmp/stale-capture',
                   FO2_RT_LIVE_DIAGNOSTIC='1')
        return subprocess.run([str(app/'Contents/MacOS/FlatOut2'), *args], env=env,
                              capture_output=True, text=True, timeout=30)

    def test_relocation_preserves_profile_and_owned_assets(self):
        with tempfile.TemporaryDirectory(dir=self.root) as temp:
            root = Path(temp)
            app = self.make_app(root)
            data = root/'User data with spaces'
            first = self.launch(app, data, '--prepare-only')
            self.assertEqual(first.returncode, 0, first.stderr)
            saves = data/'gamedata/Savegame'
            (saves/'player001.sav').write_bytes(b'friend-profile')
            (saves/'device.cfg').write_bytes(b'friend-device-settings')
            moved = root/'Moved app.app'
            app.rename(moved)
            again = self.launch(moved, data, '--prepare-only')
            self.assertEqual(again.returncode, 0, again.stderr)
            for name in ASSETS:
                self.assertFalse((data/'gamedata'/name).is_symlink())
                self.assertEqual((data/'gamedata'/name).read_bytes(), ('fixture '+name).encode())
            self.assertEqual((saves/'player001.sav').read_bytes(), b'friend-profile')
            self.assertEqual((saves/'device.cfg').read_bytes(), b'friend-device-settings')
            self.assertFalse((moved/'Contents/Resources/gamedata/Savegame').exists())

    def test_release_ssr_environment_and_forwarded_arguments(self):
        with tempfile.TemporaryDirectory(dir=self.root) as temp:
            root = Path(temp)
            app = self.make_app(root)
            data = root/'user'
            run = self.launch(app, data, '--lang=English')
            self.assertEqual(run.returncode, 0, run.stderr)
            text = next((data/'logs').glob('*/play.log')).read_text()
            for expected in ['view=ssr', 'ssr_materials=1', 'ssr_capture=0', 'rt=0',
                             'scenario=manual', 'auto=unset', 'shaders=unset', '--windowed --cap60 --lang=English']:
                self.assertIn(expected, text)

    def test_relative_override_is_rejected_without_creating_data(self):
        with tempfile.TemporaryDirectory(dir=self.root) as temp:
            app = self.make_app(Path(temp))
            run = self.launch(app, Path('relative-data'), '--prepare-only')
            self.assertEqual(run.returncode, 1)
            self.assertIn('absolute directory', run.stderr)


if __name__ == '__main__':
    unittest.main()
