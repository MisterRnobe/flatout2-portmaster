import importlib.util
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('water_physics', ROOT/'scripts/research_water_physics.py')
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)


class CollisionWaterTest(unittest.TestCase):
    def fixture(self, kind, raw=38):
        first = bytes([0, 3, 0, 48, 0, 0])
        bodies = {0: first+bytes([0, raw, 0, 0, 12, 0, 192, 0, 0]),
                  1: first+bytes([0, 0, 0, 12, 0, 192, 0, 0]),
                  2: bytes([0, raw, 0, 3, 6])*2,
                  3: bytes([0, 0, 3, 6])*2,
                  4: bytes([0, 0, 12, 192, 0])*2,
                  5: bytes([0, 96, 24])*2}
        tri = bodies[kind]
        header = bytearray(64)
        header[:4] = b'!vq\x98'
        struct.pack_into('<3i', header, 20, 10, 0, 10)
        struct.pack_into('<6f', header, 32, 1, 1, 1, 1, 1, 1)
        struct.pack_into('<2I', header, 56, 8, 8+len(tri))
        leaf = struct.pack('<2I', 3 | 6 << 2 | kind << 6, 2 | raw << 7)
        return header + leaf + tri + struct.pack('<9h', 0, 0, 0, 10, 0, 0, 0, 0, 10)

    def water(self):
        return {39: dict(name='Water (water)', RayCollision=True)}

    def test_all_encodings_and_subsequent_wide_records(self):
        for kind in range(6):
            result = mod.inspect_collision(self.fixture(kind), self.water())
            self.assertEqual(result['triangles'], 2)
            self.assertEqual(len(result['water_triangles']), 2)
            for tri in result['water_triangles']:
                self.assertEqual(tri['points'], [(0., 0., 0.), (10., 0., 0.), (0., 0., 10.)])
                self.assertEqual(tri['raw_surface_code'], 38)
                self.assertEqual(tri['query_mask'], 6)

    def test_does_not_treat_wet_ground_as_water(self):
        result = mod.inspect_collision(self.fixture(3), {39: dict(name='Wet Mud', RayCollision=True)})
        self.assertEqual(result['water_triangles'], [])
        result = mod.inspect_collision(self.fixture(3), {39: dict(name='Water (water)', RayCollision=False)})
        self.assertEqual(result['water_triangles'], [])

    def test_invalid_ranges_tree_and_quantization(self):
        for offset, value in [(56, 7), (60, 0xFFFFFFFF), (64, 0), (32, 0x7FC00000)]:
            data = self.fixture(3)
            struct.pack_into('<I', data, offset, value)
            with self.assertRaises(ValueError):
                mod.inspect_collision(data, self.water())
        with self.assertRaises(ValueError):
            mod.inspect_collision(self.fixture(3)[:-2], self.water())
        with self.assertRaises(ValueError):
            list(mod.triangle_records(b'\0', 7, 1, 0, 0))

    def test_authored_surfaces_and_optional_ray_property(self):
        text = b'''Surfaces = {
 [1] = {
 Name = "Ground",
 BodyCollision = true,
 CameraCollision = true,
 },
 [2] = {
 Name = "Water (water)",
 BodyCollision = false,
 CameraCollision = true,
 RayCollision = true,
 },
}'''
        rows = mod.surface_definitions(text)
        self.assertFalse(rows[1]['RayCollision'])
        self.assertTrue(rows[2]['RayCollision'])
        self.assertFalse(rows[2]['BodyCollision'])
        with self.assertRaises(ValueError):
            mod.surface_definitions(text.replace(b'[2]', b'[1]'))


if __name__ == '__main__':
    unittest.main()
