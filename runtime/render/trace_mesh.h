#pragma once
#include "commands.h"
#include <algorithm>
#include <cstring>
#include <unordered_map>
namespace fo2render {
enum class TraceMeshStatus { Ready,Empty,Unsupported,Invalid,Limit };
inline const char* traceMeshStatusName(TraceMeshStatus s){
  switch(s){case TraceMeshStatus::Ready:return "ready";case TraceMeshStatus::Empty:return "empty";
    case TraceMeshStatus::Unsupported:return "unsupported";case TraceMeshStatus::Invalid:return "invalid";default:return "limit";}
}
struct TraceMeshLimits {uint32_t inputElements=300000,vertices=100000,triangles=100000;};
struct TraceMesh {
  // Raw POSITION input only. No world transform or vertex deformation is inferred.
  std::vector<std::array<float,3>> positions;
  std::vector<uint32_t> indices;
  std::array<float,3> minimum{},maximum{};
  uint32_t degenerate=0,restarts=0;
};
struct TraceMeshResult {TraceMeshStatus status=TraceMeshStatus::Empty;TraceMesh mesh;};
inline TraceMeshResult prepareTraceMesh(const Draw& d,TraceMeshLimits limits={}){
  TraceMeshResult out;
  auto fail=[&](TraceMeshStatus status){return TraceMeshResult{status,{}};};
  if(!d.count||!d.instanceCount)return out;
  if(!d.vs || d.instanceCount!=1 || d.perInstanceMask ||
     (d.primitive!=Primitive::Triangles&&d.primitive!=Primitive::TriangleStrip))return fail(TraceMeshStatus::Unsupported);
  if(d.count>limits.inputElements)return fail(TraceMeshStatus::Limit);
  if(d.count<3)return out;
  if(d.primitive==Primitive::Triangles && d.count%3)return fail(TraceMeshStatus::Invalid);
  uint64_t maximumTriangles=d.primitive==Primitive::Triangles?d.count/3:d.count-2;
  if(maximumTriangles>limits.triangles)return fail(TraceMeshStatus::Limit);
  const Attribute* position=nullptr;
  for(const auto& input:d.vs->inputs)if(input[1]==0 && input[2]==0){
    if(position)return fail(TraceMeshStatus::Unsupported);
    for(const auto& a:d.attributes)if(a.slot==input[0]){
      if(position)return fail(TraceMeshStatus::Invalid);position=&a;
    }
  }
  if(!position)return fail(TraceMeshStatus::Unsupported);
  const auto a=*position;
  if(a.stream>=d.streams.size() || !d.streams[a.stream])return fail(TraceMeshStatus::Invalid);
  if(a.type!=Element::Float3 && a.type!=Element::Float4)return fail(TraceMeshStatus::Unsupported);
  const uint64_t elementBytes=a.type==Element::Float3?12:16,stride=d.strides[a.stream];
  if(!stride || uint64_t(a.offset)+elementBytes>stride)return fail(TraceMeshStatus::Unsupported);
  const auto& bytes=d.streams[a.stream]->bytes;
  if(d.indices && (d.indexSize!=2&&d.indexSize!=4))return fail(TraceMeshStatus::Invalid);
  if(d.indices && (uint64_t(d.indexStart)+d.count)*d.indexSize>d.indices->bytes.size())return fail(TraceMeshStatus::Invalid);
  auto& mesh=out.mesh;
  mesh.positions.reserve(std::min(d.count,limits.vertices));mesh.indices.reserve(size_t(maximumTriangles)*3);
  std::unordered_map<uint64_t,uint32_t> remap;remap.reserve(std::min(d.count,limits.vertices));
  auto read=[&](uint64_t source,uint32_t& dest)->TraceMeshStatus{
    auto found=remap.find(source);if(found!=remap.end()){dest=found->second;return TraceMeshStatus::Ready;}
    uint64_t start=uint64_t(d.offsets[a.stream])+a.offset;
    if(start>bytes.size() || elementBytes>bytes.size()-start || source>(bytes.size()-start-elementBytes)/stride)
      return TraceMeshStatus::Invalid;
    std::array<float,4> v={0,0,0,1};std::memcpy(v.data(),bytes.data()+start+source*stride,size_t(elementBytes));
    for(float f:v)if(!std::isfinite(f))return TraceMeshStatus::Invalid;
    if(v[3]!=1)return TraceMeshStatus::Unsupported;
    if(mesh.positions.size()>=limits.vertices)return TraceMeshStatus::Limit;
    dest=uint32_t(mesh.positions.size());mesh.positions.push_back({v[0],v[1],v[2]});remap.emplace(source,dest);
    return TraceMeshStatus::Ready;
  };
  std::array<uint32_t,3> triangle{};uint32_t filled=0,parity=0;
  for(uint32_t i=0;i<d.count;++i){
    uint64_t source=uint64_t(d.vertexStart)+i;
    if(d.indices){
      uint32_t raw=0;std::memcpy(&raw,d.indices->bytes.data()+(uint64_t(d.indexStart)+i)*d.indexSize,d.indexSize);
      if(d.primitive==Primitive::TriangleStrip && raw==(d.indexSize==2?0xffffu:0xffffffffu)){
        filled=parity=0;++mesh.restarts;continue;
      }
      int64_t adjusted=int64_t(raw)+d.baseVertex;if(adjusted<0)return fail(TraceMeshStatus::Invalid);source=uint64_t(adjusted);
    }
    uint32_t vertex;auto status=read(source,vertex);if(status!=TraceMeshStatus::Ready)return fail(status);
    triangle[filled++]=vertex;if(filled<3)continue;
    auto t=triangle;if(d.primitive==Primitive::TriangleStrip && (parity&1))std::swap(t[0],t[1]);
    const auto&p=mesh.positions[t[0]];const auto&q=mesh.positions[t[1]];const auto&r=mesh.positions[t[2]];
    std::array<double,3> u{},v{},cross{};double scale=0;
    for(unsigned k=0;k<3;++k){u[k]=double(q[k])-p[k];v[k]=double(r[k])-p[k];scale+=u[k]*u[k]+v[k]*v[k];}
    for(unsigned k=0;k<3;++k)cross[k]=u[(k+1)%3]*v[(k+2)%3]-u[(k+2)%3]*v[(k+1)%3];
    double area=0;for(double f:cross)area+=f*f;
    if(area<=scale*scale*1e-20)++mesh.degenerate;
    else mesh.indices.insert(mesh.indices.end(),t.begin(),t.end());
    if(d.primitive==Primitive::TriangleStrip){triangle[0]=triangle[1];triangle[1]=triangle[2];filled=2;++parity;}
    else filled=0;
  }
  if(mesh.indices.empty()){out.status=TraceMeshStatus::Empty;return out;}
  mesh.minimum=mesh.maximum=mesh.positions[mesh.indices.front()];
  for(auto index:mesh.indices)for(unsigned k=0;k<3;++k){mesh.minimum[k]=std::min(mesh.minimum[k],mesh.positions[index][k]);mesh.maximum[k]=std::max(mesh.maximum[k],mesh.positions[index][k]);}
  out.status=TraceMeshStatus::Ready;return out;
}
inline std::vector<uint32_t> traceSourceIndices(const Draw& d,size_t vertices){
  std::vector<uint32_t> sources;std::unordered_map<uint32_t,uint32_t> seen;
  if(d.count>100000)throw std::runtime_error("Trace mesh source index budget");
  for(uint32_t i=0;i<d.count;++i){uint64_t source=uint64_t(d.vertexStart)+i;
    if(d.indices){
      if((d.indexSize!=2&&d.indexSize!=4)||(uint64_t(d.indexStart)+i+1)*d.indexSize>d.indices->bytes.size())throw std::runtime_error("Trace mesh source index bounds");
      uint32_t raw=0;std::memcpy(&raw,d.indices->bytes.data()+(uint64_t(d.indexStart)+i)*d.indexSize,d.indexSize);
      if(d.primitive==Primitive::TriangleStrip&&raw==(d.indexSize==2?0xffffu:0xffffffffu))continue;
      int64_t adjusted=int64_t(raw)+d.baseVertex;if(adjusted<0)throw std::runtime_error("Trace mesh negative base vertex");source=uint64_t(adjusted);
    }
    if(source>UINT32_MAX)throw std::runtime_error("Trace mesh source vertex overflow");
    if(seen.emplace(uint32_t(source),uint32_t(sources.size())).second)sources.push_back(uint32_t(source));
  }
  if(sources.size()!=vertices)throw std::runtime_error("Trace mesh source normalization mismatch");return sources;
}

}
