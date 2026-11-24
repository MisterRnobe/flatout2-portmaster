import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RTSceneLauncherTest(unittest.TestCase):
    def launch(self, args=(), result=0, snapshot=True, water_result=0, dedicated=False, gain=None):
        with tempfile.TemporaryDirectory(prefix='fo2-rt-launcher-') as temp:
            root = Path(temp)
            for name in ['TestMetalWater.command', 'TestMetal.command', 'TestMetalRTRace.command']:
                shutil.copy2(ROOT/name, root/name)
            (root/'scripts').mkdir()
            shutil.copy2(ROOT/'scripts/prepare_test_scenario.py', root/'scripts/prepare_test_scenario.py')
            if dedicated or '--auto-race' in args:
                (root/'gamedata/Savegame').mkdir(parents=True)
                (root/'gamedata/Savegame/profile').write_text('original')
                for name in ['FlatOut2.exe', 'd3dx9_30.dll', 'filesystem', 'data.bfs', 'zoom_platform.bfs']:
                    (root/'gamedata'/name).write_text(name)
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
            play.write_text('#!/bin/bash\necho "play:$FO2_TEST_SCENARIO:$FO2_DEBUG_VIEW" >> "$FO2_FAKE_LOG"\necho "live:$FO2_RT_LIVE_DIAGNOSTIC" >> "$FO2_FAKE_LOG"\necho "water:$FO2_RT_WATER" >> "$FO2_FAKE_LOG"\n'
                            'echo "level:${FO2_TEST_LEVEL:-manual}" >> "$FO2_FAKE_LOG"\n'
                            'echo "gain:${FO2_WATER_REFLECTION_GAIN:-1}" >> "$FO2_FAKE_LOG"\n'
                            'if [[ "$FO2_TEST_SCENARIO" == auto-race ]]; then\n'
                            '  [[ "$FO2_GAME_DIR" -ef "$FO2_METAL_SESSION_DIR/gamedata" ]] || exit 9\n'
                            '  echo changed > "$FO2_GAME_DIR/Savegame/profile"\nfi\n')
            play.chmod(0o755)
            if snapshot:
                scene = root/'work/metal/runs/old run/rt-view-scene'
                scene.mkdir(parents=True)
                (scene/'scene.json').write_text('{}')
            env = dict(os.environ, FO2_FAKE_LOG=str(root/'calls'), FO2_FAKE_RT_RESULT=str(result), FO2_FAKE_WATER_RESULT=str(water_result), FO2_WATER_PHYSICS_TRACE='0')
            for name in ['FO2_RT_SCENE_DIR', 'FO2_METAL_SESSION_DIR', 'FO2_MEMORY_CHECK']:
                env.pop(name, None)
            if gain is None: env.pop('FO2_WATER_REFLECTION_GAIN', None)
            else: env['FO2_WATER_REFLECTION_GAIN'] = str(gain)
            run = subprocess.run(['bash', str(root/('TestMetalRTRace.command' if dedicated else 'TestMetalWater.command')), *args], env=env,
                                 capture_output=True, text=True, timeout=30)
            self.assertTrue((root/'calls').exists(), run.stdout+run.stderr)
            if dedicated or '--auto-race' in args:
                self.assertEqual((root/'gamedata/Savegame/profile').read_text(), 'original')
            return run, (root/'calls').read_text()

    def test_dedicated_race_uses_rt_and_isolated_derby(self):
        run, calls = self.launch(dedicated=True)
        self.assertEqual(run.returncode, 0, run.stdout+run.stderr)
        self.assertIn('play:auto-race:ssr', calls)
        self.assertIn('level:water-derby', calls)
        self.assertIn('water:1', calls)
        self.assertIn('gain:4', calls)
        self.assertIn('--metal-check-raytracing-water', calls)

    def test_dedicated_race_can_restore_unit_gain(self):
        run, calls = self.launch(dedicated=True, gain=1)
        self.assertEqual(run.returncode, 0, run.stdout+run.stderr)
        self.assertIn('gain:1', calls)
        run, calls = self.launch()
        self.assertEqual(run.returncode, 0, run.stdout+run.stderr)
        self.assertIn('gain:1', calls)

    def test_automatic_race_preserves_mask_and_rt_fallback(self):
        run, calls = self.launch(('--auto-race', '--rt-water', '--mask'), water_result=77)
        self.assertEqual(run.returncode, 0, run.stdout+run.stderr)
        self.assertIn('play:auto-race:ssr-mask', calls)
        self.assertIn('level:water-derby', calls)
        self.assertIn('water:0', calls)

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
