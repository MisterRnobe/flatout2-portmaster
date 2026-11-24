"""Exercise the native bounded snapshot loader/reference through its CPU CLI."""
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get('FO2_TEST_BINARY', ROOT/'work/metal/fo2-metal'))


class TraceSceneLoaderTest(unittest.TestCase):
    def run_scene(self, change=None):
        with tempfile.TemporaryDirectory(prefix='fo2-scene-') as temp:
            root = Path(temp)
            scene = root/'scene with spaces'
            scene.mkdir()
            doc = dict(schema=1, status='cpu-prepared', coordinate_space='right-handed-view',
                       coverage='submitted-captured-draws-only', vertices=3, triangles=1,
                       source_frame=123, projection=[1, 1, 1.01, -.1])
            files = {'positions.f32': struct.pack('<9f', -1, -1, -2, 1, -1, -2, 0, 1, -2),
                     'indices.u32': struct.pack('<3I', 0, 1, 2), 'triangle-mesh.u32': struct.pack('<I', 0)}
            if change:
                change(doc, files)
            (scene/'scene.json').write_text(json.dumps(doc))
            for name, data in files.items():
                (scene/name).write_bytes(data)
            env = dict(os.environ, FO2_METAL_SESSION_DIR=str(root/'report'))
            run = subprocess.run([str(BINARY), '--metal-check-raytracing-scene-cpu', str(scene)],
                                 env=env, capture_output=True, text=True, timeout=30)
            report = json.loads((root/'report/raytracing-scene-check.json').read_text())
            return run, report

    def test_prepared_scene_and_cpu_truth(self):
        run, report = self.run_scene()
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(report['status'], 'cpu-validated')
        self.assertGreater(report['reference_hits'], 0)
        self.assertFalse(report['native_rt_gpu_validated'])
        self.assertFalse(report['gameplay_rt_enabled'])
        self.assertEqual(report['source_frame'], 123)

    def test_reject_bad_metadata_and_binary_inputs(self):
        changes = [
            lambda d, f: d.update(coordinate_space='world'),
            lambda d, f: d.update(vertices=250001),
            lambda d, f: d.update(vertices=3.5),
            lambda d, f: d.update(projection=[0, 1, 1.01, -.1]),
            lambda d, f: d.update(projection='invalid'),
            lambda d, f: f.update({'positions.f32': bytes(12)}),
            lambda d, f: f.update({'indices.u32': struct.pack('<3I', 0, 1, 3)}),
            lambda d, f: f.update({'positions.f32': struct.pack('<9f', float('nan'), *([0]*8))}),
            lambda d, f: f.update({'triangle-mesh.u32': struct.pack('<I', 2048)}),
            lambda d, f: f.update({'indices.u32': bytes(250000*12+1)}),
        ]
        for change in changes:
            with self.subTest(change=change):
                run, report = self.run_scene(change)
                self.assertEqual(run.returncode, 1, run.stdout)
                self.assertEqual(report['status'], 'failed')
                self.assertFalse(report['native_rt_gpu_validated'])

    def test_expanded_bounded_scene_and_mesh_ids(self):
        def expanded(doc, files):
            doc['vertices'] = 100001
            files['positions.f32'] += bytes((100001-3)*12)
            files['triangle-mesh.u32'] = struct.pack('<I', 1500)
        run, report = self.run_scene(expanded)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(report['vertices'], 100001)


if __name__ == '__main__':
    unittest.main()
