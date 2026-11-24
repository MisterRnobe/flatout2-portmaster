#include "../runtime/render/trace_capture.h"
#include "../runtime/render/shader_capture.h"
#include <cassert>
using namespace fo2render;
static Draw fixture(){
  Draw d;d.vs=std::make_shared<Shader>();d.vs->inputs={{0,0,0}};
  d.vs->source="vertex capture fixture";d.vs->hash=shaderHash(d.vs->source.data(),d.vs->source.size());
  d.vsConstants={1,2,3,4};d.attributes={{0,0,0,Element::Float3}};d.strides[0]=12;
  d.streams[0]=std::make_shared<Buffer>();float v[]={0,0,0,1,0,0,0,1,0};
  d.streams[0]->bytes.resize(sizeof v);std::memcpy(d.streams[0]->bytes.data(),v,sizeof v);d.count=3;return d;
}
int main(int argc,char**argv){
  assert(argc==2);std::filesystem::path root=argv[1];std::filesystem::create_directories(root);
  auto d=fixture();TraceCapture disabled;disabled.present(true,1);disabled.record(d);assert(!disabled.enabled());
  TraceCaptureLimits limits;limits.warmupFrames=2;
  TraceCapture capture(root/"complete",limits);capture.record(d);assert(!std::filesystem::exists(root/"complete"));
  capture.present(true,0);capture.record(d);assert(!std::filesystem::exists(root/"complete"));
  capture.present(false,1);capture.present(true,10);capture.record(d);assert(!std::filesystem::exists(root/"complete"));
  capture.present(true,11);capture.record(d);
  d.streams[0]->bytes.assign(d.streams[0]->bytes.size(),0);d.vs->source="changed";d.vsConstants={9};
  capture.present(true,12);assert(!capture.enabled());
  auto stamp=std::filesystem::last_write_time(root/"complete/manifest.json");
  capture.record(fixture());capture.present(true,13);assert(std::filesystem::last_write_time(root/"complete/manifest.json")==stamp);
  limits.warmupFrames=0;limits.draws=1;
  TraceCapture limited(root/"limited",limits);limited.present(true,0);limited.record(fixture());limited.record(fixture());limited.present(true,1);
  limits.draws=128;TraceCapture invalid(root/"invalid",limits);invalid.present(true,0);
  d=fixture();d.vertexStart=100;invalid.record(d);invalid.present(true,1);
  std::ofstream(root/"blocker")<<"unchanged";
  TraceCapture io(root/"blocker/capture",limits);io.present(true,0);io.record(fixture());assert(!io.enabled());
  std::puts("PASS: opt-in warmup/reset, one-frame ownership, budgets, invalid geometry and contained I/O failure");
}
