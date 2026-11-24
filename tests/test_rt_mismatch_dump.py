import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MismatchDumpTest(unittest.TestCase):
    def test_exact_scene_ray_and_hit_are_saved_without_a_gpu(self):
        with tempfile.TemporaryDirectory(prefix='fo2-mismatch-') as temp:
            root = Path(temp)
            source = root/'dump.mm'
            source.write_text('''#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "runtime/d3d9_metal/raytracing_check.h"
#include "runtime/d3d9_metal/trace_scene_check.h"
#include "runtime/d3d9_metal/live_trace_check.h"
#include "runtime/d3d9_metal/trace_mismatch_dump.h"
int main(int argc,char** argv){@autoreleasepool{try{
  using namespace fo2render;if(argc>2)return replayTraceMismatch(argv[1]);LiveTraceSnapshot s;s.frame=42;s.sample=1;
  s.scene.positions={{{-1,-1,-2}},{{1,-1,-2}},{{0,1,-2}}};s.scene.indices={0,1,2};s.scene.meshIds={0};s.scene.projection={1,1,1.01f,-.1f};
  std::vector<TraceRay> rays={{{0,0,0,0},{0,0,-1,10},{255,0,0,0}}};
  auto expected=traceSceneReference(s.scene,rays[0]);auto actual=expected;actual.barycentric[0]+=.02f;
  auto reason=traceSceneHitMismatch(s.scene,rays[0],actual,expected);if(!reason)return 2;
  saveTraceMismatch(s,rays,&actual,0,expected,reason,argv[1]);return 0;
}catch(const std::exception& e){std::fprintf(stderr,"%s\\n",e.what());return 1;}}}
''')
            binary = root/'dump'
            subprocess.run([os.environ.get('CXX', '/opt/homebrew/opt/llvm/bin/clang++'), '-std=c++17', '-fobjc-arc',
                            '-fsanitize=address,undefined', '-I'+str(ROOT), str(source), '-framework', 'Foundation',
                            '-framework', 'Metal', '-o', str(binary)], check=True, capture_output=True, text=True)
            output = root/'saved scene'
            subprocess.run([str(binary), str(output)], check=True,
                           env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0:halt_on_error=1'))
            doc = json.loads((output/'mismatch.json').read_text())
            self.assertEqual(doc['reason'], 'barycentric')
            self.assertEqual(doc['source_frame'], 42)
            self.assertEqual(doc['ray_stride'], 48)
            self.assertEqual(doc['hit_stride'], 32)
            self.assertEqual(doc['actual_triangle'], doc['expected_triangle'])
            self.assertEqual(doc['actual']['distance'], doc['expected_nearest']['distance'])
            self.assertAlmostEqual(doc['actual']['u']-doc['expected_nearest']['u'], .02, places=5)
            self.assertEqual(len((output/'rays.bin').read_bytes()), 48)
            self.assertEqual(len((output/'hits.bin').read_bytes()), 32)
            self.assertEqual(struct.unpack('<fIII4f', (output/'hits.bin').read_bytes())[3], 1)
            self.assertEqual((output/'indices.u32').read_bytes(), struct.pack('<3I', 0, 1, 2))
            self.assertFalse(json.loads((output/'scene.json').read_text())['native_rt_gpu_validated'])
            env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:halt_on_error=1')
            replay = subprocess.run([str(binary), str(output), 'replay'], env=env, capture_output=True, text=True)
            self.assertEqual(replay.returncode, 1)  # A real wrong barycentric stays rejected.
            report = json.loads((output/'replay-report.json').read_text())
            self.assertEqual(report['mismatches'], 1)
            self.assertFalse(report['gpu_executed'])
            good = bytearray((output/'hits.bin').read_bytes())
            struct.pack_into('<f', good, 16, .25)
            (output/'hits.bin').write_bytes(good)
            replay = subprocess.run([str(binary), str(output), 'replay'], env=env, capture_output=True, text=True)
            self.assertEqual(replay.returncode, 0, replay.stderr)
            (output/'hits.bin').write_bytes(good[:-1])
            replay = subprocess.run([str(binary), str(output), 'replay'], env=env, capture_output=True, text=True)
            self.assertEqual(replay.returncode, 1)
            self.assertIn('ABI/count mismatch', replay.stderr)


if __name__ == '__main__':
    unittest.main()
