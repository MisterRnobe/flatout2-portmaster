import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('check_trace_capture', ROOT / 'scripts/check_trace_capture.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class CaptureTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='FO2 trace capture ')
        cls.path = Path(cls.temp.name)
        binary = cls.path / 'capture-check'
        subprocess.run(['clang++', '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined',
                        str(ROOT / 'tests/trace_capture_check.cpp'), '-o', str(binary)], check=True)
        subprocess.run([str(binary), str(cls.path / 'captures')], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_roundtrip_and_ownership(self):
        folder = self.path / 'captures/complete'
        data = checker.validate(folder)
        self.assertEqual((data['status'], data['meshes'], data['vertices'], data['triangles']), ('complete', 1, 3, 1))
        self.assertEqual((folder / 'mesh-0.constants.bin').read_bytes(), bytes([1, 2, 3, 4]))
        doc = json.loads((folder / 'manifest.json').read_text())
        self.assertEqual(doc['frame'], 12)

    def test_bounded_and_invalid(self):
        self.assertEqual(checker.validate(self.path / 'captures/limited')['status'], 'limited')
        self.assertEqual(checker.validate(self.path / 'captures/invalid')['meshes'], 0)
        self.assertEqual((self.path / 'captures/blocker').read_text(), 'unchanged')

    def test_corrupt_index_and_shader(self):
        for name, filename, content in [('index', 'mesh-0.indices.u32', struct.pack('<III', 0, 1, 99)),
                                        ('shader', 'mesh-0.vertex.metal', b'x' * len(b'vertex capture fixture'))]:
            dest = self.path / name
            shutil.copytree(self.path / 'captures/complete', dest)
            (dest / filename).write_bytes(content)
            with self.assertRaises(ValueError):
                checker.validate(dest)

    def test_transform_contract(self):
        dest = self.path / 'wrong-transform'
        shutil.copytree(self.path / 'captures/complete', dest)
        path = dest / 'manifest.json'
        doc = json.loads(path.read_text()); doc['world_space_validated'] = True
        path.write_text(json.dumps(doc))
        with self.assertRaises(ValueError):
            checker.validate(dest)


if __name__ == '__main__':
    unittest.main()
