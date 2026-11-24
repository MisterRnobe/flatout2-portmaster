#pragma once
#include "trace_mesh.h"
#include <list>
namespace fo2render {
struct CachedTraceMesh {TraceMeshStatus status=TraceMeshStatus::Empty;std::shared_ptr<const TraceMesh> mesh;bool hit=false;};
// Raw geometry only. Matrices are deliberately excluded: a cached mesh must
// still be transformed using each current draw's constants.
class TraceMeshCache {
  struct Entry {std::string key;std::weak_ptr<Buffer> vertex,index;std::shared_ptr<const TraceMesh> mesh;size_t bytes;};
  std::list<Entry> lru;std::unordered_map<std::string,typename std::list<Entry>::iterator> lookup;
  size_t used=0,byteLimit,entryLimit;
  void erase(typename std::list<Entry>::iterator it){used-=it->bytes;lookup.erase(it->key);lru.erase(it);}
  static bool keyFor(const Draw& d,std::string& key,std::shared_ptr<Buffer>& vertex){
    if(!d.vs||d.attributes.size()>32||d.vs->inputs.size()>32)return false;
    const Attribute* position=nullptr;
    for(auto input:d.vs->inputs)if(input[1]==0&&input[2]==0){
      if(position)return false;for(const auto& a:d.attributes)if(a.slot==input[0]){if(position)return false;position=&a;}
    }
    if(!position||position->stream>=d.streams.size())return false;
    vertex=d.streams[position->stream];if(!vertex||vertex->transient||(d.indices&&d.indices->transient))return false;
    key.reserve(160);
    auto put=[&](uint64_t value){key.append(reinterpret_cast<const char*>(&value),sizeof value);};
    for(auto b:{vertex,d.indices}){put(reinterpret_cast<uintptr_t>(b.get()));put(b?b->version:0);put(b?b->bytes.size():0);}
    put(position->slot);put(position->stream);put(position->offset);put(unsigned(position->type));
    put(d.strides[position->stream]);put(d.offsets[position->stream]);put(d.count);put(unsigned(d.primitive));
    put(d.vertexStart);put(d.indexStart);put(d.indexSize);put(uint32_t(d.baseVertex));put(d.instanceCount);put(d.perInstanceMask);
    return true;
  }
public:
  explicit TraceMeshCache(size_t bytes=32*1024*1024,size_t entries=2048):byteLimit(bytes),entryLimit(entries){}
  TraceMeshCache(const TraceMeshCache&)=delete;
  TraceMeshCache& operator=(const TraceMeshCache&)=delete;
  void clear(){lookup.clear();lru.clear();used=0;}
  size_t bytes()const{return used;}size_t entries()const{return lru.size();}
  CachedTraceMesh prepare(const Draw& d,TraceMeshLimits limits){
    if(!d.count||!d.instanceCount)return {};
    if(!d.vs||d.instanceCount!=1||d.perInstanceMask||(d.primitive!=Primitive::Triangles&&d.primitive!=Primitive::TriangleStrip))return {TraceMeshStatus::Unsupported,{}};
    if(d.count>limits.inputElements)return {TraceMeshStatus::Limit,{}};
    uint64_t maximum=d.count<3?0:d.primitive==Primitive::Triangles?d.count/3:d.count-2;
    if(maximum>limits.triangles)return {TraceMeshStatus::Limit,{}};
    std::string key;std::shared_ptr<Buffer> vertex;bool cacheable=keyFor(d,key,vertex);
    if(cacheable){auto found=lookup.find(key);if(found!=lookup.end()){
      auto it=found->second;
      // Weak ownership prevents a new buffer at a reused address/version from
      // inheriting an earlier allocation's mesh. No native buffer is retained.
      auto sameOwner=[](const std::weak_ptr<Buffer>& old,const std::shared_ptr<Buffer>& now){return !old.owner_before(now)&&!now.owner_before(old);};
      if(!sameOwner(it->vertex,vertex)||!sameOwner(it->index,d.indices))erase(it);
      else {auto mesh=it->mesh;lru.splice(lru.begin(),lru,it);
        if(mesh->positions.size()>limits.vertices||mesh->indices.size()/3>limits.triangles)return {TraceMeshStatus::Limit,{}};
        return {TraceMeshStatus::Ready,std::move(mesh),true};}
    }}
    auto raw=prepareTraceMesh(d,limits);if(raw.status!=TraceMeshStatus::Ready)return {raw.status,{}};
    auto mesh=std::make_shared<TraceMesh>(std::move(raw.mesh));size_t bytes=mesh->positions.capacity()*12+mesh->indices.capacity()*4;
    if(cacheable&&entryLimit&&bytes<=byteLimit){
      while(!lru.empty()&&(lru.size()>=entryLimit||used>byteLimit-bytes))erase(std::prev(lru.end()));
      lru.push_front({key,vertex,d.indices,mesh,bytes});
      try{lookup.emplace(key,lru.begin());}catch(...){lru.pop_front();throw;}
      used+=bytes;
    }
    return {TraceMeshStatus::Ready,std::move(mesh),false};
  }
};
}
