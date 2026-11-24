#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
namespace fo2test {
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
