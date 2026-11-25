import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('startup_summary', Path(__file__).resolve().parents[1]/'scripts/summarize_startup.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class StartupSummaryTest(unittest.TestCase):
    def test_interleaved_surfaces_failed_lock_and_first_present_boundary(self):
        report = module.summarize('''[t+1.000s] [FO2 STARTUP D3D] enter token=7e0050d0 object=00000001 caller=00a01234
[t+1.010s] [FO2 STARTUP D3D] leave token=7e0050d0 object=00000001 result=00000000 bridge_ms=10.000
[t+1.020s] [FO2 STARTUP D3D] enter token=7e0050d0 object=00000002 caller=00a02345
[t+1.030s] [FO2 STARTUP D3D] leave token=7e0050d0 object=00000002 result=8876086c bridge_ms=10.000
[t+2.000s] [FO2 STARTUP D3D] enter token=7e0050e0 object=00000002 caller=00a03456
[t+2.010s] [FO2 STARTUP D3D] leave token=7e0050e0 object=00000002 result=8876086c bridge_ms=10.000
[t+7.010s] [FO2 STARTUP D3D] enter token=7e0050e0 object=00000001 caller=00a04567
[t+7.030s] [FO2 STARTUP D3D] leave token=7e0050e0 object=00000001 result=00000000 bridge_ms=20.000
[FO2 PIPELINE] frame=0 new state 12.34ms vs=1 ps=2
[FO2 STARTUP] first Metal present 8.5 seconds after renderer initialization
[FO2 PIPELINE] frame=0 new state 999.0ms vs=1 ps=2
''')
        self.assertEqual(report['first_present_renderer_seconds'], 8.5)
        self.assertEqual(report['logged_slow_frame_zero_pipeline_ms'], 12.34)
        self.assertEqual(len(report['surface_bridge_calls']), 4)
        self.assertEqual(report['surface_guest_intervals'], [{'object': '00000001', 'lock_caller': '00a01234', 'unlock_caller': '00a04567', 'guest_interval_seconds': 6.0}])
        self.assertEqual(report['unpaired_surface_entries'], 0)

    def test_sparse_old_logs_are_not_fabricated_surface_measurements(self):
        report = module.summarize('[t+0.7s] [NFSU2] D3D9 COM kind=5 method=13\n[t+7.1s] [NFSU2] D3D9 COM kind=5 method=14\n')
        self.assertIsNone(report['first_present_renderer_seconds'])
        self.assertEqual(report['surface_guest_intervals'], [])
        self.assertEqual(report['largest_sparse_log_gaps'][0]['seconds'], 6.4)

    def test_play_summary_and_failed_diagnostic_preserve_game_exit(self):
        source = Path(__file__).resolve().parents[1]
        for result, broken_report in ((0, False), (7, False), (7, True)):
            with self.subTest(result=result, broken_report=broken_report), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                shutil.copy2(source/'PlayMetal.command', root/'PlayMetal.command')
                (root/'scripts').mkdir()
                report = root/'scripts/summarize_startup.py'
                shutil.copy2(source/'scripts/summarize_startup.py', report)
                if broken_report:
                    report.write_text('raise RuntimeError("diagnostic unavailable")\n')
                fake = root/'fake-game'
                fake.write_text(f'#!/bin/bash\necho "[FO2 STARTUP] first Metal present 2.5 seconds after renderer initialization"\nexit {result}\n')
                fake.chmod(0o755)
                session = root/'session'
                env = dict(os.environ, FO2_METAL_BINARY=str(fake), FO2_METAL_SESSION_DIR=str(session), FO2_STARTUP_TRACE='1', FO2_NET_TRACE='0')
                run = subprocess.run(['bash', str(root/'PlayMetal.command')], env=env, capture_output=True, text=True, timeout=10)
                self.assertEqual(run.returncode, result, run.stdout+run.stderr)
                self.assertTrue((session/'play.log').exists())
                self.assertEqual((session/'startup-summary.json').exists(), not broken_report)
                self.assertEqual(len(list((root/'work/metal/failures').glob('*.log'))) if result else 0, 1 if result else 0)


if __name__ == '__main__':
    unittest.main()
