#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <unordered_set>
#include <unordered_map>
#include <vector>
namespace fo2offcamera {
struct Node {uint32_t address=0,world=0,car=0,partState=0;};
struct Selection {std::vector<Node> nodes;uint32_t cars=0,visited=0,visible=0,hidden=0,distant=0,unsupported=0;
  uint32_t parts=0,looseRoots=0,releasedRoots=0,coveredParts=0,partMeshes=0;};
// Verified against this EXE's RenderViews, mesh-node vtable/method and car
// hierarchy. Read-only discovery; the caller submits through the guest's own
// mesh method only after all ordinary mesh queues have been populated.
template<class Reader> Selection select(uint32_t context,uint32_t camera,Reader read,uint32_t buckets=0){
  auto u32=[&](uint32_t base,uint32_t offset=0){uint32_t v=0;if(base>UINT32_MAX-offset||!read(base+offset,&v,4))throw std::runtime_error("Offcamera pointer bounds");return v;};
  auto matrix=[&](uint32_t base,uint32_t offset){std::array<float,16> m{};if(base>UINT32_MAX-offset||base+offset>UINT32_MAX-64||!read(base+offset,m.data(),64))throw std::runtime_error("Offcamera matrix bounds");
    for(float v:m)if(!std::isfinite(v)||std::abs(v)>1e6f)throw std::runtime_error("Offcamera invalid matrix");
    if(std::abs(m[3])>1e-5f||std::abs(m[7])>1e-5f||std::abs(m[11])>1e-5f||std::abs(m[15]-1)>1e-5f)throw std::runtime_error("Offcamera non-affine matrix");return m;};
  Selection result;if(u32(context,0x1bc)!=1) return result; // Split views need separate provenance.
  if(u32(0x67ae30,4)!=0x54cd20)throw std::runtime_error("Offcamera mesh ABI mismatch");
  auto cam=matrix(camera,0x80);
  uint32_t begin=u32(context,0x1ac),end=u32(context,0x1a8);
  if(!begin||end<begin||(end-begin)%8||(end-begin)/8>16384)throw std::runtime_error("Offcamera visible list bounds");
  std::unordered_set<uint32_t> visible;
  for(uint32_t p=begin;p<end;p+=8)visible.insert(u32(p));
  std::unordered_set<uint32_t> queuedWorlds;
  if(buckets){
    if(u32(context,0x1b4)!=buckets||buckets>UINT32_MAX-20*16)throw std::runtime_error("Offcamera bucket identity");
    size_t entries=0;
    for(unsigned i=0;i<20;++i){uint32_t slot=buckets+i*16,b=u32(slot),e=u32(slot,4),capacity=u32(slot,8);
      if(e<b||capacity<e||(e-b)%16||(e-b)/16>8192||(entries+=(e-b)/16)>32768)throw std::runtime_error("Offcamera ordinary queue bounds");
      for(uint32_t p=b;p<e;p+=16)queuedWorlds.insert(u32(p,4));
    }
  }
  uint32_t race=u32(0x696dc8);if(!race)return result;
  // The game switches to just the last active player in certain presentations.
  // Defer those and menu/garage lists rather than inventing a population.
  if(u32(race,0x208a0))return result;
  begin=u32(race,0x14);end=u32(race,0x18);
  if(!begin||end<begin||(end-begin)%4||(end-begin)/4>32)throw std::runtime_error("Offcamera player list bounds");
  std::unordered_set<uint32_t> visited;
  struct Pending {uint32_t node,car,depth;bool inheritedHidden,sibling,inheritedVisible;};
  auto walk=[&](uint32_t root,uint32_t car,uint32_t partState){
    if(!root||visited.count(root))return;
    std::vector<Pending> stack{{root,car,0,false,false,false}};
    while(!stack.empty()){auto item=stack.back();stack.pop_back();if(!item.node)continue;
      if(item.depth>64||visited.size()>=4096||!visited.insert(item.node).second)throw std::runtime_error("Offcamera cyclic/oversized hierarchy");
      ++result.visited;uint32_t flags=u32(item.node,0x10),child=u32(item.node,0xc4),sibling=u32(item.node,0xc8);
      if(item.sibling&&sibling)stack.push_back({sibling,car,item.depth,item.inheritedHidden,true,item.inheritedVisible});
      bool hidden=item.inheritedHidden||(flags&2);
      bool ordinary=item.inheritedVisible||visible.count(item.node);
      if(child)stack.push_back({child,car,item.depth+1,hidden,true,ordinary});
      if(hidden){++result.hidden;continue;}
      if(u32(item.node)!=0x67ae30)continue;
      if(ordinary||queuedWorlds.count(item.node+0x80)){++result.visible;continue;}
      // The alternate instanced-node queue uses a different transform payload.
      uint32_t models=u32(item.node,0x108);if(!models||models>8||u32(item.node,0x118)){++result.unsupported;continue;}
      auto world=matrix(item.node,0x80);double d2=0;for(unsigned k=0;k<3;++k){double d=double(world[12+k])-cam[12+k];d2+=d*d;}
      if(d2>80.*80.){++result.distant;continue;}
      if(result.nodes.size()<128){result.nodes.push_back({item.node,item.node+0x80,car,partState});if(partState)++result.partMeshes;}
    }
  };
  std::unordered_set<uint32_t> cars;
  for(uint32_t p=begin;p<end;p+=4){uint32_t player=u32(p);if(!player)continue;uint32_t car=u32(player,0x33c);if(!car||!cars.insert(car).second)continue;++result.cars;
    walk(u32(car,0x338),car,0);
    // 0x42898D/0x4366F0 unlink the node for states 1 (loose) and 2
    // (released). Its world matrix is subsequently driven by the part's body,
    // not reconstructed from the car's transform or a previous frame.
    uint32_t partsBegin=u32(car,0x1bec),partsEnd=u32(car,0x1bf0);
    if(partsEnd<partsBegin||(partsEnd-partsBegin)%4||(partsEnd-partsBegin)/4>128||(!partsBegin&&partsEnd))
      throw std::runtime_error("Offcamera part list bounds");
    std::unordered_set<uint32_t> parts;
    for(uint32_t at=partsBegin;at<partsEnd;at+=4){uint32_t part=u32(at);if(!part)continue;
      if(!parts.insert(part).second){++result.coveredParts;continue;}
      ++result.parts;uint32_t state=u32(part,0x10);if(!state)continue;
      if(state>2||u32(part,0xfc)!=car){++result.unsupported;continue;}
      uint32_t node=u32(part,0x2b0);if(!node)continue;
      if(visited.count(node)){++result.coveredParts;continue;}
      // Only an independent root is admitted. Never detach/reparent a node
      // on behalf of RT, and never follow its root-level world siblings.
      if(u32(node,0xc0)){++result.unsupported;continue;}
      if(state==1)++result.looseRoots;else ++result.releasedRoots;
      walk(node,car,state);
    }
  }return result;
}
// World identity is an address, never a floating-point matrix comparison:
// distinct visible and hidden nodes may have the same transform.
class Provenance {
  std::unordered_map<uint32_t,uint32_t> worlds;
  uint32_t current=0,depth=0;
  std::array<bool,64> eligible{};
public:
  void reset(){worlds.clear();current=depth=0;}
  void add(uint32_t world,uint32_t kind=1){if(kind==1||kind==2)worlds.emplace(world,kind);}
  void setWorld(uint32_t world){current=world;}
  void enter(bool primary=true){if(depth<eligible.size())eligible[depth]=primary;++depth;}
  void leave(){if(depth)--depth;}
  uint32_t kind()const{if(!depth||depth>eligible.size()||!eligible[depth-1]||!current)return 0;
    auto found=worlds.find(current);return found==worlds.end()?0:found->second;}
  bool rayOnly()const{return kind()!=0;}
};
}
