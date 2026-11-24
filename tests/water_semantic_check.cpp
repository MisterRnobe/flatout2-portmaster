#include "../runtime/guest/water_semantic_trace.h"
#include <cassert>
#include <cstdio>
#include <unistd.h>
using namespace fo2water;
int main(){
  std::vector<uint8_t> settings(0x1D31),physics(0x20724),material(0x90);
  uint32_t settingsPointer=0x100000,materialPointer=0x300000,shader=25;
  settings[0x1D30]=1;std::memcpy(settings.data()+0x1D2C,&materialPointer,4);std::memcpy(material.data()+0x8C,&shader,4);
  Signature a;a.asset=123;for(size_t i=0;i<a.bytes.size();++i)a.bytes[i]=uint8_t(i+1);
  std::memcpy(physics.data()+0x780,a.bytes.data(),48);
  auto reader=[&](uint32_t at,void* out,size_t count){
    if(at==0x8DA758&&count==4){std::memcpy(out,&settingsPointer,4);return true;}
    for(auto pair:{std::pair<uint32_t,std::vector<uint8_t>*>{0x100000,&settings},{0x200000,&physics},{0x300000,&material}}){
      if(at>=pair.first&&uint64_t(at)-pair.first+count<=pair.second->size()){
        std::memcpy(out,pair.second->data()+at-pair.first,count);return true;}}
    return false;
  };
  auto result=observe(0x200000,{a},reader);
  assert(result.settingsReadable&&result.physicsReadable&&result.materialReadable&&result.waterPlane==1&&result.shader==25);
  assert(result.collision.size()==1&&result.collision[0].asset==123&&result.collision[0].offset==0x780);
  // A signature collision is ambiguous: retain both identities, never choose
  // a level or enable a water material from this diagnostic match alone.
  auto b=a;b.asset=456;assert(observe(0x200000,{a,b},reader).collision.size()==2);
  std::fill(physics.begin()+0x780,physics.begin()+0x780+48,0);
  std::memcpy(material.data(),a.bytes.data(),48);std::memcpy(physics.data()+0x1000,&materialPointer,4);
  auto indirect=observe(0x200000,{a},reader);
  assert(indirect.collision.size()==1&&indirect.collision[0].indirect&&indirect.collision[0].object==materialPointer&&indirect.collision[0].offset==0x1000);
  std::memcpy(physics.data()+0xF718,&materialPointer,4);
  auto primary=observe(0x200000,{a},reader);assert(activeCollisionAsset(primary)==124);
  auto directReader=[&](uint32_t at,void* out,size_t size){assert(size<=48);return reader(at,out,size);};
  auto direct=observe(0x200000,{a},directReader,false);assert(activeCollisionAsset(direct)==124&&direct.pointerProbes==1);
  std::fill(material.begin(),material.begin()+48,0);
  settings[0x1D30]=0;assert(observe(0x200000,{a},reader).waterPlane==0);
  settings[0x1D30]=2;assert(!observe(0x200000,{a},reader).settingsReadable);
  settingsPointer=UINT32_MAX;assert(!observe(0x200000,{a},reader).settingsReadable);
  assert(!observe(UINT32_MAX,{a},reader).physicsReadable);
  assert(!observe(0,{a},reader).physicsReadable);
  physics[0x780]^=1;assert(observe(0x200000,{a},reader).collision.empty());
  Observation identities;identities.collision={{123,0x780,0x100000,false},{456,0x1000,0x200000,true}};
  assert(activeCollisionAsset(identities)==0);
  identities.collision.push_back({123,0xF718,0x300000,true});assert(activeCollisionAsset(identities)==124);
  identities.collision.push_back({456,0xF718,0x300000,true});assert(activeCollisionAsset(identities)==0);
  char path[]="/tmp/fo2-water-signatures-XXXXXX";int fd=mkstemp(path);assert(fd>=0);close(fd);
  std::ostringstream text;text<<a.asset<<' ';const char* hex="0123456789abcdef";
  for(auto byte:a.bytes)text<<hex[byte>>4]<<hex[byte&15];text<<'\n';
  {std::ofstream file(path);file<<text.str();}
  auto loaded=loadSignatures(path);assert(loaded.size()==1&&loaded[0].bytes==a.bytes&&loaded[0].asset==123);
  for(const auto& bad:{text.str()+text.str(),std::string("12 zzz\n"),std::string("# empty\n")}){
    {std::ofstream file(path);file<<bad;}
    bool rejected=false;try{loadSignatures(path);}catch(const std::exception&){rejected=true;}assert(rejected);
  }
  unlink(path);
  std::puts("PASS: read-only water semantics, loaded collision signatures, ambiguity preservation, bounds/flag/readability rejection");
}
