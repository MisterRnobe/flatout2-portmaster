"""Compare the live CPU collector against the independently prepared real capture."""
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LiveCaptureMatchTest(unittest.TestCase):
    def test_current_transform_matches_offline_scene(self):
        captures = [p for p in (ROOT/'work/metal/runs').glob('*/rt-view-scene/scene.json')
                    if (p.parent.parent/'rt-geometry/manifest.json').exists()]
        if not captures:
            self.skipTest('No real raw/prepared capture pair available')
        prepared = max(captures, key=lambda p: p.stat().st_mtime).parent
        raw = prepared.parent/'rt-geometry'
        manifest = json.loads((raw/'manifest.json').read_text())
        code = ['#include "runtime/render/live_trace_scene.h"', '#include <fstream>', '#include <map>',
                'using namespace fo2render;',
                'Bytes read(const std::string& p){std::ifstream f(p,std::ios::binary);return Bytes(std::istreambuf_iterator<char>(f),{});}',
                'int main(int argc,char**argv){LiveTraceScene live(true,0,0,{},0,true);live.present(true,0);std::vector<Draw> draws;',
                'std::map<uint32_t,std::shared_ptr<Image>> images;',
                'auto image=[&](uint32_t id){auto& p=images[id];if(!p)p=std::make_shared<Image>();return p;};']
        for mesh in manifest['meshes']:
            stem = str(raw/mesh['stem'])
            uniforms = ','.join('{%d,%d,%d}' % (u['kind'], u['index'], u['count']) for u in mesh['uniforms'])
            projection = ','.join(repr(v) for v in mesh['projection_estimate'])
            code += ['{ Draw d;d.vs=std::make_shared<Shader>();d.vs->inputs={{0,0,0}};',
                     f'd.vs->hash={mesh["vertex_shader_hash"]}ull;d.vs->uniforms={{{uniforms}}};',
                     f'auto source=read({json.dumps(stem+".vertex.metal")});d.vs->source.assign(source.begin(),source.end());',
                     f'd.vsConstants=read({json.dumps(stem+".constants.bin")});',
                     f'd.debugProjection={{{{{projection}}},{str(mesh["projection_estimate_valid"]).lower()}}};',
                     'd.streams[0]=std::make_shared<Buffer>();d.indices=std::make_shared<Buffer>();',
                     f'd.streams[0]->bytes=read({json.dumps(stem+".positions.f32")});d.indices->bytes=read({json.dumps(stem+".indices.u32")});',
                     'd.strides[0]=12;d.attributes={{0,0,0,Element::Float3}};d.indexSize=4;',
                     f'd.count={mesh["triangles"]*3};d.color=image({mesh["color_target"]});d.depth=image({mesh["depth_target"]});live.record(d);draws.push_back(d);}}']
        code += ['auto cold=live.present(true,1);if(!cold||cold->prepared||!cold->scene.positions.empty())return 1;finalizeLiveTraceSnapshot(*cold);',
                 'live.present(true,2);for(const auto& d:draws)live.record(d);auto result=live.present(true,3);',
                 'if(!result||result->cacheHits!=cold->accepted||result->cacheMisses)return 2;finalizeLiveTraceSnapshot(*result);',
                 'if(result->scene.positions!=cold->scene.positions||result->scene.indices!=cold->scene.indices||result->fingerprint!=cold->fingerprint)return 3;',
                 'std::printf("Capture cold/warm: %.3f/%.3f ms; warm hits=%u misses=%u, worker prepare %.3f ms\\n",cold->collectNs/1e6,result->collectNs/1e6,result->cacheHits,result->cacheMisses,result->prepareNs/1e6);',
                 'std::ofstream p(std::string(argv[1])+"/positions.f32",std::ios::binary),i(std::string(argv[1])+"/indices.u32",std::ios::binary);',
                 'p.write((const char*)result->scene.positions.data(),result->scene.positions.size()*12);',
                 'i.write((const char*)result->scene.indices.data(),result->scene.indices.size()*4);return (!p||!i)?1:0;}']
        with tempfile.TemporaryDirectory(prefix='fo2-live-match-') as temp:
            folder = Path(temp)
            source = folder/'match.cpp'
            source.write_text('\n'.join(code))
            subprocess.run([os.environ.get('CXX', '/opt/homebrew/opt/llvm/bin/clang++'), '-std=c++17', '-O1',
                            '-fsanitize=address,undefined', '-I'+str(ROOT), str(source), '-o', str(folder/'match')], check=True)
            subprocess.run([str(folder/'match'), str(folder)], check=True,
                           env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0:halt_on_error=1'))
            def expanded(root):
                positions = list(struct.iter_unpack('<3f', (root/'positions.f32').read_bytes()))
                return [positions[i[0]] for i in struct.iter_unpack('<I', (root/'indices.u32').read_bytes())]
            actual, expected = expanded(folder), expanded(prepared)
            self.assertEqual(len(actual), len(expected))
            maximum = 0
            for a, b in zip(actual, expected):
                for x, y in zip(a, b):
                    error = abs(x-y)/max(1, abs(x), abs(y))
                    maximum = max(maximum, error)
                    self.assertLess(error, 3e-4)
            print(f'Real live/offline match: {len(actual)//3} triangles, maximum relative error {maximum:.3g}')


if __name__ == '__main__':
    unittest.main()
