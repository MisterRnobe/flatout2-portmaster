#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
namespace fo2test {
// Appended only to the locally inspected SingleplayerMenu stream in the
// isolated water scenario. Use the game's single-race event path directly;
// never confirm the default Career item or infer success from arbitrary physics.
class AutoRaceMenuScript {
  bool enabled_,derby_;uint32_t buffer_=0;size_t offset_=0;
public:
  explicit AutoRaceMenuScript(bool enabled,bool derby=false):enabled_(enabled),derby_(derby){}
  std::string read(uint32_t buffer,std::string_view chunk){
    if(!enabled_)return {};
    if(!chunk.empty()){
      if(chunk.find("--// SingleplayerMenu.bed")<128){buffer_=buffer;offset_=0;}
      return {};
    }
    if(!buffer_||buffer_!=buffer)return {};
    const std::string id=derby_?"57":"16";
    const std::string path=derby_?"data/Tracks/Derby/Derby6/A/":"data/Tracks/Canal/Canal1/A/";
    const std::string text=
      "\nif menu_singleplayer and menu_singleplayer.update then\n"
      "local previous=menu_singleplayer.update\n"
      "menu_singleplayer.update=function(self,time)\nprevious(self,time)\n"
      "if not FO2TestRaceStarted and Levels and Levels["+id+"] and Levels["+id+"].StagePath==\""+path+"\" then\n"
      "local cars=CarlistAllUnlocked()\nif cars and cars[1] and cars[1].index then\n"
      "FO2TestRaceStarted=true\nGameFlow.ClearRace()\n"
      "db.GameFlow.PreRace.Mode=GM_SINGLE_RACE\n"
      "db.GameFlow.PreRace.Car=cars[1].index\n"
      "db.GameFlow.PreRace.CarSkin=1\ndb.GameFlow.PreRace.Level="+id+"\n"
      "Event:PostEvent(event(EVENT_RACE_BEGIN))\nend\nend\nend\nend\n";
    if(offset_>=text.size()){buffer_=0;return {};}
    auto out=text.substr(offset_,512);offset_+=out.size();return out;
  }
};
class WaterLevelScript {
  bool enabled_,injected_=false;uint32_t buffer_=0;size_t offset_=0;
public:
  explicit WaterLevelScript(bool enabled):enabled_(enabled){}
  bool injected()const{return injected_;}
  std::string read(uint32_t buffer,std::string_view chunk){
    if(!enabled_)return {};
    if(!chunk.empty()){
      if(chunk.find("BaseLevelPath = \"data/Tracks/\"")<8 && chunk.find("TRACKTYPE_FOREST")!=std::string_view::npos){buffer_=buffer;offset_=0;injected_=false;}
      return {};
    }
    if(!buffer_||buffer_!=buffer)return {};
    // Keep the first menu slot/type, but use the verified Canal 1 data row.
    // No Lua library functions or changes to the original archives are needed.
    static const std::string text=
      "\nif Levels and Levels[1] and Levels[16] and Levels[16].StagePath==\"data/Tracks/Canal/Canal1/A/\" then\n"
      "local a=Levels[1] local b=Levels[16]\n"
      "a.Name=b.Name a.Description=b.Description a.GfxSetPath=b.GfxSetPath a.TrackPath=b.TrackPath a.StagePath=b.StagePath\n"
      "a.TrackImage=b.TrackImage a.TrackIcon=b.TrackIcon a.TrackLogo=b.TrackLogo a.Laps=1 a.AIHandicapLevel=b.AIHandicapLevel\nend\n";
    if(offset_>=text.size()){buffer_=0;return {};}
    auto out=text.substr(offset_,512);offset_+=out.size();injected_=offset_==text.size();return out;
  }
};
}
