"""Race tests must never modify the player's saves."""
import importlib.util
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('scenario', Path(__file__).resolve().parents[1] / 'scripts/prepare_test_scenario.py')
scenario = importlib.util.module_from_spec(spec)
spec.loader.exec_module(scenario)

class ScenarioFilesTest(unittest.TestCase):
    def test_isolation_and_repeatable_seed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / 'source'
            (source / 'Savegame').mkdir(parents=True)
            for name in (*scenario.ASSETS, 'filesystem'):
                (source / name).write_text(name)
            profile = source / 'Savegame/profile'
            profile.write_text('original')
            seed = root / 'seed'
            first = scenario.prepare(source, root / 'first', seed)
            (first / 'Savegame/profile').write_text('test changed save')
            self.assertEqual(profile.read_text(), 'original')
            profile.write_text('later player save')
            second = scenario.prepare(source, root / 'second', seed)
            self.assertEqual((second / 'Savegame/profile').read_text(), 'original')
            for name in scenario.ASSETS:
                self.assertFalse((second / name).is_symlink())
                self.assertEqual((second / name).read_bytes(), (source / name).read_bytes())
                self.assertNotEqual((second / name).stat().st_ino, (source / name).stat().st_ino)
            (second / 'data.bfs').write_text('test asset write')
            self.assertEqual((source / 'data.bfs').read_text(), 'data.bfs')
            with self.assertRaises(ValueError):
                scenario.prepare(source, root / 'second', seed)
            (source / 'data.bfs').unlink()
            with self.assertRaises(ValueError):
                scenario.prepare(source, root / 'third', seed)

if __name__ == '__main__':
    unittest.main()
