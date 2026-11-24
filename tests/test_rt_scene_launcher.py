import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RTSceneLauncherTest(unittest.TestCase):
    def launch(self, args=(), result=0, snapshot=True, water_result=0):
        with tempfile.TemporaryDirectory(prefix='fo2-rt-launcher-') as temp:
            root = Path(temp)
            for name in ['TestMetalWater.command', 'TestMetal.command']:
                shutil.copy2(ROOT/name, root/name)
            # The agent sandbox denies /dev/fd process substitution. Capture
            # output with subprocess instead; exercise the actual launch logic.
            common = root/'TestMetal.command'
            common.write_text('\n'.join(line for line in common.read_text().splitlines()
                                        if not line.startswith('exec > >(tee '))+'\n')
            binary = root/'work/metal/fo2-metal'
            binary.parent.mkdir(parents=True)
            binary.write_text('''#!/bin/bash
if [[ "$1" == --metal-check-raytracing-water ]]; then
    echo "$1" >> "$FO2_FAKE_LOG"
    exit "$FO2_FAKE_WATER_RESULT"
fi
if [[ "$1" == --metal-check-raytracing-scene ]]; then
    echo "scene:$2" >> "$FO2_FAKE_LOG"
    exit "$FO2_FAKE_RT_RESULT"
fi
echo "$1" >> "$FO2_FAKE_LOG"
''')
            binary.chmod(0o755)
            play = root/'PlayMetal.command'
            play.write_text('#!/bin/bash\necho "play:$FO2_TEST_SCENARIO:$FO2_DEBUG_VIEW" >> "$FO2_FAKE_LOG"\necho "live:$FO2_RT_LIVE_DIAGNOSTIC" >> "$FO2_FAKE_LOG"\necho "water:$FO2_RT_WATER" >> "$FO2_FAKE_LOG"\n')
            play.chmod(0o755)
            if snapshot:
                scene = root/'work/metal/runs/old run/rt-view-scene'
                scene.mkdir(parents=True)
                (scene/'scene.json').write_text('{}')
            env = dict(os.environ, FO2_FAKE_LOG=str(root/'calls'), FO2_FAKE_RT_RESULT=str(result), FO2_FAKE_WATER_RESULT=str(water_result))
            for name in ['FO2_RT_SCENE_DIR', 'FO2_METAL_SESSION_DIR', 'FO2_MEMORY_CHECK']:
                env.pop(name, None)
            run = subprocess.run(['bash', str(root/'TestMetalWater.command'), *args], env=env,
                                 capture_output=True, text=True, timeout=30)
            self.assertTrue((root/'calls').exists(), run.stdout+run.stderr)
            return run, (root/'calls').read_text()

    def test_water_mode_preflight_and_fallback(self):
        for result in (0, 77, 1):
            run, calls = self.launch(('--rt-water', '--no-rt-scene'), water_result=result)
            self.assertIn('--metal-check-raytracing-water', calls)
            if result == 1:
                self.assertEqual(run.returncode, 1)
                self.assertNotIn('play:', calls)
            else:
                self.assertEqual(run.returncode, 0, run.stdout+run.stderr)
                self.assertIn('water:'+('1' if result == 0 else '0'), calls)
                self.assertIn('live:0', calls)
                self.assertIn('play:manual:ssr', calls)

    def test_default_auto_scene_keeps_manual_gameplay(self):
        run, calls = self.launch()
        self.assertEqual(run.returncode, 0, run.stdout+run.stderr)
        self.assertIn('old run/rt-view-scene', calls)
        self.assertIn('play:manual:ssr', calls)
        self.assertIn('live:1', calls)

    def test_live_diagnostic_can_be_disabled(self):
        run, calls = self.launch(('--no-rt-live',))
        self.assertEqual(run.returncode, 0, run.stdout+run.stderr)
        self.assertIn('live:0', calls)

    def test_skip_unsupported_and_absent_snapshot(self):
        for args, result, snapshot in [((), 77, True), (('--no-rt-scene',), 1, True), ((), 0, False)]:
            run, calls = self.launch(args, result, snapshot)
            self.assertEqual(run.returncode, 0, run.stdout+run.stderr)
            self.assertIn('play:manual:ssr', calls)
            if args or not snapshot:
                self.assertNotIn('scene:', calls)

    def test_real_error_stops_before_gameplay(self):
        run, calls = self.launch(result=1)
        self.assertEqual(run.returncode, 1)
        self.assertNotIn('play:', calls)

    def test_explicit_path_and_combined_flags(self):
        run, calls = self.launch(('--mask', '--rt-check', '--rt-scene', '/tmp/prepared scene'))
        self.assertEqual(run.returncode, 0, run.stdout+run.stderr)
        self.assertIn('scene:/tmp/prepared scene', calls)
        self.assertIn('--metal-check-raytracing\n', calls)
        self.assertIn('play:manual:ssr-mask', calls)


if __name__ == '__main__':
    unittest.main()
