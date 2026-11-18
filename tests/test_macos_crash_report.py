"""Verify the native signal reporter without requiring a desktop or game boot."""
import pathlib
import platform
import re
import signal
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

@unittest.skipUnless(platform.system() == 'Darwin', 'macOS signal context')
class CrashReportTest(unittest.TestCase):
    def test_sigill_has_pc_and_image_base(self):
        result = subprocess.run([str(ROOT/'work/macos/fo2'), str(ROOT/'gamedata'),
                                 '--check-crash-handler'], capture_output=True, text=True)
        self.assertIn(result.returncode, [-signal.SIGILL, 128+signal.SIGILL])
        record = re.search(r'\[FO2 FAULT\].*', result.stderr)
        self.assertIsNotNone(record, result.stderr)
        for field in ['pc', 'lr', 'sp', 'image']:
            value = re.search(r'\b'+field+r'=0x([0-9a-f]+)', record.group())
            self.assertIsNotNone(value, record.group())
            self.assertGreater(int(value[1], 16), 0)

if __name__ == '__main__':
    unittest.main()
