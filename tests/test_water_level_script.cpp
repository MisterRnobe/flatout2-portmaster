#include "../runtime/guest/test_level_script.h"
#include <cassert>
#include <cstdio>
int main(){
  const std::string header="\r\nBaseLevelPath = \"data/Tracks/\"\r\nTRACKTYPE_FOREST = 1\r\nLevels = {";
  fo2test::WaterLevelScript script(true);
  assert(script.read(1,"unrelated Lua").empty()&&script.read(1,"").empty());
  assert(script.read(2,header).empty());assert(script.read(3,"").empty());
  auto injected=script.read(2,"");assert(!injected.empty()&&injected.size()<=512&&script.injected());
  assert(injected.find("Levels[16].StagePath==\"data/Tracks/Canal/Canal1/A/\"")!=std::string::npos);
  assert(injected.find("a.StagePath=b.StagePath")!=std::string::npos);
  assert(injected.find("a.TrackType=")==std::string::npos);
  assert(script.read(2,"").empty());
  fo2test::WaterLevelScript off(false);assert(off.read(2,header).empty()&&off.read(2,"").empty()&&!off.injected());
  std::puts("Water-level script checks passed: exact header, EOF/buffer matching, bounded one-time injection, type preservation and disabled mode");
}
