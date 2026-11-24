#pragma once
#include "trace_mesh.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <locale>
#include <map>
#include <sstream>
namespace fo2render {
struct TraceCaptureLimits {
  uint32_t warmupFrames=120,draws=128,inputElements=400000,vertices=100000,triangles=100000;
  size_t payloadBytes=8*1024*1024;
};
// One bounded diagnostic frame. Exported meshes remain in shader-input space.
// Errors disable the diagnostic; they must never terminate gameplay.
class TraceCapture {
  std::filesystem::path directory;
  TraceCaptureLimits limits;
  uint32_t warmup=0,attempts=0,vertices=0,triangles=0,inputWork=0;
  uint64_t frameNumber=0,skippedAfterLimit=0,degenerate=0;
  size_t payload=0;
  bool armed=false,active=false,done=false,limited=false;
  std::array<uint32_t,5> statuses{};
  std::vector<std::string> records;
  std::map<std::weak_ptr<Image>,uint32_t,std::owner_less<std::weak_ptr<Image>>> targets;
  uint32_t targetId(const std::shared_ptr<Image>& image){
    if(!image)return 0;
    return targets.emplace(std::weak_ptr<Image>(image),uint32_t(targets.size()+1)).first->second;
  }
  void write(const std::string& name,const void* data,size_t size){
    std::ofstream file(directory/name,std::ios::binary);file.exceptions(std::ios::badbit|std::ios::failbit);
    if(size)file.write(static_cast<const char*>(data),std::streamsize(size));file.close();
  }
  void finish(){
    std::ostringstream json;json.imbue(std::locale::classic());json.precision(9);
    json<<"{\n\"schema\":1,\"status\":\""<<(records.empty()?"no-mesh":limited?"limited":"complete")<<"\","
      <<"\"coordinate_space\":\"raw-position-input\",\"world_space_validated\":false,\"vertex_shader_evaluated\":false,"
      <<"\"byte_order\":\"little-endian\",\"frame\":"<<frameNumber<<",\"draws_attempted\":"<<attempts
      <<",\"skipped_after_limit\":"<<skippedAfterLimit<<",\"vertices\":"<<vertices<<",\"triangles\":"<<triangles
      <<",\"degenerate_triangles\":"<<degenerate<<",\"input_elements_budgeted\":"<<inputWork<<",\"payload_bytes\":"<<payload
      <<",\"limits\":{\"draws\":"<<limits.draws<<",\"input_elements\":"<<limits.inputElements
      <<",\"vertices\":"<<limits.vertices<<",\"triangles\":"<<limits.triangles<<",\"payload_bytes\":"<<limits.payloadBytes<<"},\"results\":{";
    for(unsigned i=0;i<statuses.size();++i){if(i)json<<',';json<<'"'<<traceMeshStatusName(TraceMeshStatus(i))<<"\":"<<statuses[i];}
    json<<"},\"meshes\":[";for(unsigned i=0;i<records.size();++i){if(i)json<<',';json<<records[i];}json<<"]}\n";
    auto text=json.str();write("manifest.json.tmp",text.data(),text.size());
    std::filesystem::rename(directory/"manifest.json.tmp",directory/"manifest.json");
    std::printf("[FO2 RT GEOMETRY] %zu raw meshes, %u vertices, %u triangles, %zu payload bytes%s: %s\n",
      records.size(),vertices,triangles,payload,limited?" (limited)":"",(directory/"manifest.json").c_str());
    done=true;armed=active=false;
  }
  void fail(const std::exception& error) noexcept {
    done=true;armed=active=false;
    std::fprintf(stderr,"[FO2 RT GEOMETRY] Capture disabled after error: %s\n",error.what());
  }
public:
  explicit TraceCapture(std::filesystem::path path={},TraceCaptureLimits budget={}) : directory(std::move(path)),limits(budget){}
  bool enabled()const{return !directory.empty()&&!done;}
  void present(bool racing,uint64_t frame) noexcept {
    if(!enabled())return;
    try{
      if(active){finish();return;}
      if(!racing){warmup=0;armed=false;return;}
      if(warmup<limits.warmupFrames)++warmup;
      armed=warmup>=limits.warmupFrames;frameNumber=frame+1;
    }catch(const std::exception& error){fail(error);}
  }
  void record(const Draw& d) noexcept {
    if(!enabled()||!armed)return;
    active=true;
    if(attempts>=limits.draws||inputWork>=limits.inputElements){limited=true;++skippedAfterLimit;return;}
    ++attempts;
    try{
      std::filesystem::create_directories(directory);
      auto result=prepareTraceMesh(d,{limits.inputElements-inputWork,limits.vertices-vertices,limits.triangles-triangles});
      inputWork+=std::min(d.count,limits.inputElements-inputWork);
      if(result.status==TraceMeshStatus::Ready && (d.vs->source.size()>128*1024 || d.vsConstants.size()>16*1024))result.status=TraceMeshStatus::Limit;
      auto& mesh=result.mesh;
      size_t bytes=mesh.positions.size()*12+mesh.indices.size()*4+(d.vs?d.vs->source.size():0)+d.vsConstants.size();
      if(result.status==TraceMeshStatus::Ready && bytes>limits.payloadBytes-payload)result.status=TraceMeshStatus::Limit;
      ++statuses[unsigned(result.status)];degenerate+=mesh.degenerate;
      if(result.status==TraceMeshStatus::Limit)limited=true;
      if(result.status!=TraceMeshStatus::Ready)return;
      std::string stem="mesh-"+std::to_string(records.size());
      write(stem+".positions.f32",mesh.positions.data(),mesh.positions.size()*12);
      write(stem+".indices.u32",mesh.indices.data(),mesh.indices.size()*4);
      write(stem+".vertex.metal",d.vs->source.data(),d.vs->source.size());
      write(stem+".constants.bin",d.vsConstants.data(),d.vsConstants.size());
      std::ostringstream json;json.imbue(std::locale::classic());json.precision(9);
      json<<"{\"stem\":\""<<stem<<"\",\"vertices\":"<<mesh.positions.size()<<",\"triangles\":"<<mesh.indices.size()/3
        <<",\"source_elements\":"<<d.count<<",\"source_primitive\":\""<<(d.primitive==Primitive::Triangles?"triangles":"triangle-strip")
        <<"\",\"vertex_shader_hash\":\""<<d.vs->hash<<"\",\"pixel_shader_hash\":\""<<(d.ps?d.ps->hash:0)
        <<"\",\"material_cue\":"<<unsigned(d.material.cue)<<",\"cull\":"<<d.raster.cull
        <<",\"color_target\":"<<targetId(d.color)<<",\"depth_target\":"<<targetId(d.depth)
        <<",\"constant_bytes\":"<<d.vsConstants.size()<<",\"vertex_source_bytes\":"<<d.vs->source.size()
        <<",\"degenerate\":"<<mesh.degenerate<<",\"restarts\":"<<mesh.restarts<<",\"minimum\":[";
      for(unsigned k=0;k<3;++k){if(k)json<<',';json<<mesh.minimum[k];}json<<"],\"maximum\":[";
      for(unsigned k=0;k<3;++k){if(k)json<<',';json<<mesh.maximum[k];}json<<"],\"uniforms\":[";
      for(unsigned i=0;i<d.vs->uniforms.size();++i){auto u=d.vs->uniforms[i];if(i)json<<',';
        json<<"{\"kind\":"<<u.kind<<",\"index\":"<<u.index<<",\"count\":"<<u.count<<'}';}
      json<<"],\"projection_estimate_valid\":"<<(d.debugProjection.valid?"true":"false")<<",\"projection_estimate\":[";
      for(unsigned k=0;k<4;++k){if(k)json<<',';float value=d.debugProjection.values[k];if(std::isfinite(value))json<<value;else json<<"null";}
      json<<"]}";records.push_back(json.str());
      vertices+=uint32_t(mesh.positions.size());triangles+=uint32_t(mesh.indices.size()/3);payload+=bytes;
    }catch(const std::exception& error){fail(error);}
  }
};
}
