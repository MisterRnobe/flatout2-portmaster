#pragma once
// Read-only diagnostics for the verified fo2-zoom executable. No classification
// here is used to change rendering, collision, saves or guest memory.
#include <array>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <cstdint>
#include <unordered_set>
namespace fo2water {
struct Signature {uint32_t asset=0;std::array<uint8_t,48> bytes{};};
inline std::vector<Signature> loadSignatures(const std::string& path){
  std::ifstream file(path);if(!file)throw std::runtime_error("Cannot read water physics signatures");
  std::vector<Signature> result;std::string line;
  while(std::getline(file,line)){
    if(line.empty()||line[0]=='#')continue;
    if(line.size()>120||result.size()>=128)throw std::runtime_error("Water signature limit");
    std::istringstream in(line);uint32_t asset;std::string hex,extra;
    if(!(in>>asset>>hex)||asset>100000||hex.size()!=96||(in>>extra))throw std::runtime_error("Invalid water signature record");
    Signature value;value.asset=asset;
    auto digit=[](char c)->unsigned{if(c>='0'&&c<='9')return unsigned(c-'0');if(c>='a'&&c<='f')return unsigned(c-'a'+10);throw std::runtime_error("Invalid water signature hex");};
    for(size_t i=0;i<48;++i)value.bytes[i]=uint8_t(digit(hex[i*2])*16+digit(hex[i*2+1]));
    if(std::any_of(result.begin(),result.end(),[&](const auto& s){return s.asset==asset;}))throw std::runtime_error("Duplicate water signature asset");
    result.push_back(value);
  }
  if(result.empty())throw std::runtime_error("Empty water signatures");return result;
}
struct Observation {
  uint32_t settings=0,physics=0,material=0,shader=0;
  uint8_t waterPlane=0;
  bool settingsReadable=false,physicsReadable=false,materialReadable=false;
  uint32_t pointerProbes=0;bool probeLimit=false;
  struct Match {uint32_t asset,offset,object;bool indirect;};std::vector<Match> collision;
};
// The static collision query at 0x00569F09 reads world +0xF718. Only an
// unambiguous match through that member is published to render draws; other
// diagnostic matches in the broader scan are not active-level identities.
inline uint32_t activeCollisionAsset(const Observation& observation){
  uint32_t selected=0;
  for(auto match:observation.collision)if(match.indirect&&match.offset==0xF718){
    uint32_t candidate=match.asset+1;if(selected&&candidate!=selected)return 0;selected=candidate;
  }return selected;
}
// Reader validates mapped guest ranges before copying. The physics routine
// accesses through +0x20720, establishing this bounded object prefix; indirect
// pointer probes stop after one hop and 4096 unique addresses. Absence of a
// match is not dry land.
// CDB loader copies [reciprocal, scale, scalar min, scalar max] into its object.
template<class Reader> Observation observe(uint32_t physics,const std::vector<Signature>& signatures,Reader read,bool exhaustive=true){
  Observation out;out.physics=physics;
  if(read(0x008DA758u,&out.settings,4)&&out.settings&&out.settings<=UINT32_MAX-0x1D31){
    uint8_t flag=0;uint32_t material=0;
    if(read(out.settings+0x1D30,&flag,1)&&read(out.settings+0x1D2C,&material,4)&&flag<=1){
      out.waterPlane=flag;out.material=material;out.settingsReadable=true;
      if(material&&material<=UINT32_MAX-0x90)out.materialReadable=read(material+0x8C,&out.shader,4);
    }
  }
  constexpr size_t size=0x20724;
  if(!physics||physics>UINT32_MAX-size)return out;
  if(!exhaustive){
    uint32_t primary=0;if(!read(physics+0xF718,&primary,4))return out;out.physicsReadable=true;
    if(primary>=0x10000&&primary<=UINT32_MAX-48){
      ++out.pointerProbes;std::array<uint8_t,48> object;
      if(read(primary,object.data(),object.size()))for(const auto& s:signatures)
        if(object==s.bytes)out.collision.push_back({s.asset,0xF718,primary,true});
    }return out;
  }
  std::vector<uint8_t> bytes(size);
  if(!read(physics,bytes.data(),bytes.size()))return out;out.physicsReadable=true;
  for(size_t at=0;at+48<=bytes.size();at+=4)
    for(const auto& s:signatures)if(std::memcmp(bytes.data()+at,s.bytes.data(),48)==0)out.collision.push_back({s.asset,uint32_t(at),physics+uint32_t(at),false});
  // The CDB loader allocates a separate 64-byte object (caller 0x004B3B66).
  // Check one-hop fields too; quantization matches remain diagnostic only.
  std::unordered_set<uint32_t> probed;
  // Probe the known active member first: generic probe budgets or an earlier
  // alias to the same object must not hide the semantic collision pointer.
  uint32_t primary=0;std::memcpy(&primary,bytes.data()+0xF718,4);
  if(primary>=0x10000&&primary<=UINT32_MAX-48){
    probed.insert(primary);++out.pointerProbes;std::array<uint8_t,48> object;
    if(read(primary,object.data(),object.size()))for(const auto& s:signatures)
      if(object==s.bytes)out.collision.push_back({s.asset,0xF718,primary,true});
  }
  for(size_t at=0;at+4<=bytes.size();at+=4){
    uint32_t address;std::memcpy(&address,bytes.data()+at,4);
    if(address<0x10000||address>UINT32_MAX-48||!probed.insert(address).second)continue;
    if(out.pointerProbes>=4096){out.probeLimit=true;break;}++out.pointerProbes;
    std::array<uint8_t,48> object;if(!read(address,object.data(),object.size()))continue;
    for(const auto& s:signatures)if(object==s.bytes)out.collision.push_back({s.asset,uint32_t(at),address,true});
  }
  return out;
}
}
