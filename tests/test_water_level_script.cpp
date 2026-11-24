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
  fo2test::AutoRaceMenuScript menu(true),disabled(false);
  std::string menuHeader="--///////////////////////////////////////////////////////////////////////////\r\n--// SingleplayerMenu.bed\r\n";
  assert(menu.read(5,"unrelated").empty()&&menu.read(5,"").empty());
  assert(menu.read(6,menuHeader).empty()&&menu.read(7,"").empty());
  std::string payload;for(;;){auto chunk=menu.read(6,"");if(chunk.empty())break;assert(chunk.size()<=512);payload+=chunk;}
  assert(payload.find("GM_SINGLE_RACE")!=std::string::npos&&payload.find("PreRace.Level=16")!=std::string::npos);
  assert(payload.find("Car=cars[1].index")!=std::string::npos&&payload.find("EVENT_RACE_BEGIN")!=std::string::npos);
  assert(payload.find("FO2TestRaceStarted=true")!=std::string::npos&&payload.find("menu_career")==std::string::npos);
  assert(disabled.read(6,menuHeader).empty()&&disabled.read(6,"").empty());
  fo2test::AutoRaceMenuScript derby(true,true);derby.read(8,menuHeader);std::string derbyPayload;
  for(;;){auto chunk=derby.read(8,"");if(chunk.empty())break;assert(chunk.size()<=512);derbyPayload+=chunk;}
  assert(derbyPayload.find("Levels[57].StagePath==\"data/Tracks/Derby/Derby6/A/\"")!=std::string::npos);
  assert(derbyPayload.find("PreRace.Level=57")!=std::string::npos&&derbyPayload.find("GM_SINGLE_RACE")!=std::string::npos);
  std::puts("Water-level script checks passed: exact header, EOF/buffer matching, bounded one-time injection, type preservation and disabled mode");
}
