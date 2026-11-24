import math
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from prepare_trace_view_scene import matrix_rows, projection_from_matrix, transform_positions


class ViewTransformTest(unittest.TestCase):
    def test_rigid_camera_and_reprojection(self):
        # Camera forward +world X, translated by (10,20,30), non-square projection.
        rows = [[0, 0, 2, -60], [0, 3, 0, -60], [1.01, 0, 0, -10.2], [1, 0, 0, -10]]
        p = projection_from_matrix(rows)
        self.assertAlmostEqual(p[0], 2)
        self.assertAlmostEqual(p[1], 3)
        self.assertAlmostEqual(p[2], 1.01)
        self.assertAlmostEqual(p[3], -.1)
        points = transform_positions([(15, 22, 31), (9, 20, 30), (15, 20, 130)], rows, p)
        self.assertEqual(points[0], (1, 2, -5))
        self.assertEqual(points[1], (0, 0, 1))  # Behind-camera geometry remains present.
        self.assertEqual(points[2], (100, 0, -5))  # Off-screen geometry is not clipped.

    def test_packed_uniform_registers(self):
        uniforms = [dict(kind=0, index=8, count=1), dict(kind=1, index=0, count=1),
                    dict(kind=0, index=0, count=2), dict(kind=0, index=2, count=2)]
        values = list(range(20))
        data = struct.pack('<20f', *values)
        self.assertEqual(matrix_rows(uniforms, data), [tuple(range(i, i+4)) for i in [4, 8, 12, 16]])
        with self.assertRaises(ValueError):
            matrix_rows(uniforms, data[:-1])
        with self.assertRaises(ValueError):
            matrix_rows(uniforms+[dict(kind=0, index=0, count=1)], data+bytes(16))
        with self.assertRaises(ValueError):
            matrix_rows([dict(kind=0, index=0, count=3)], bytes(48))

    def test_reject_unsupported_and_nonfinite(self):
        rows = [[2, 0, 0, 0], [0, 3, 0, 0], [0, 0, 1.01, -.1], [0, 0, 1, 0]]
        projection_from_matrix(rows)
        rows[0][2] = 1
        with self.assertRaises(ValueError):
            projection_from_matrix(rows)
        bad = struct.pack('<16f', math.nan, *([0]*15))
        with self.assertRaises(ValueError):
            matrix_rows([dict(kind=0, index=0, count=4)], bad)
        rows = [[2, 0, 0, 0], [0, 3, 0, 0], [0, 0, 1.01, -.1], [0, 0, 1, 0]]
        with self.assertRaises(ValueError):
            transform_positions([(1, 1, 2)], rows, [2, 3, 1.01, -1])


if __name__ == '__main__':
    unittest.main()
