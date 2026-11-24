#pragma once
#include "live_trace_scene.h"
namespace fo2render {
using WaterMatrix=std::array<std::array<double,4>,4>;
inline WaterMatrix waterPackedMatrix(const Draw& d,unsigned first){
  WaterMatrix rows{};std::array<bool,4> found{};size_t offset=0;
  for(auto u:d.vs->uniforms)if(u.kind==0){
    if(!u.count||u.index>=256||u.count>256-u.index||offset+size_t(u.count)*16>d.vsConstants.size())throw std::runtime_error("Physical water constants invalid");
    for(unsigned r=std::max(first,u.index);r<std::min(first+4,u.index+u.count);++r){
      if(found[r-first])throw std::runtime_error("Physical water constants overlap");found[r-first]=true;
      float v[4];std::memcpy(v,d.vsConstants.data()+offset+(r-u.index)*16,16);
      for(unsigned k=0;k<4;++k){if(!std::isfinite(v[k]))throw std::runtime_error("Physical water nonfinite matrix");rows[r-first][k]=v[k];}}
    offset+=size_t(u.count)*16;
  }
  for(bool f:found)if(!f)throw std::runtime_error("Physical water matrix missing");return rows;
}
inline std::array<float,4> waterPackedConstant(const Draw& d,unsigned index){
  std::array<float,4> value{};bool found=false;size_t offset=0;
  for(auto u:d.vs->uniforms)if(u.kind==0){
    if(!u.count||u.index>=256||u.count>256-u.index||offset+size_t(u.count)*16>d.vsConstants.size())throw std::runtime_error("Physical water packed constants invalid");
    if(index>=u.index&&index<u.index+u.count){if(found)throw std::runtime_error("Physical water duplicate constant");found=true;
      std::memcpy(value.data(),d.vsConstants.data()+offset+(index-u.index)*16,16);}
    offset+=size_t(u.count)*16;
  }
  if(!found)throw std::runtime_error("Physical water required constant missing");for(float f:value)if(!std::isfinite(f))throw std::runtime_error("Physical water nonfinite constant");return value;
}
inline WaterMatrix physicalWaterBakedWorld(const Draw& d){
  auto vp=waterPackedMatrix(d,0);auto projection=traceMatrixProjection(vp);auto eye=waterPackedConstant(d,8),direction=waterPackedConstant(d,9);
  double forwardLength=0,directionLength=0,dot=0;std::array<double,3> camera{};
  for(unsigned k=0;k<3;++k){forwardLength+=vp[3][k]*vp[3][k];directionLength+=double(direction[k])*direction[k];dot+=vp[3][k]*direction[k];
    camera[k]=-vp[0][k]*vp[0][3]/(projection[0]*projection[0])-vp[1][k]*vp[1][3]/(projection[1]*projection[1])-vp[3][k]*vp[3][3];}
  if(std::abs(forwardLength-1)>.002||directionLength<.5||std::abs(dot)/std::sqrt(forwardLength*directionLength)<.995)
    throw std::runtime_error("Physical terrain is not in the verified world camera frame");
  for(unsigned k=0;k<3;++k)if(std::abs(camera[k]-eye[k])>.02+1e-5*std::abs(eye[k]))throw std::runtime_error("Physical terrain camera/world positions disagree");
  WaterMatrix world{};for(unsigned k=0;k<4;++k)world[k][k]=1;return world;
}
inline std::array<float,3> physicalWaterNormal(const Draw& d,const WaterMatrix& world){
  for(unsigned k=0;k<4;++k)if(std::abs(world[3][k]-(k==3?1:0))>1e-6)throw std::runtime_error("Physical water world transform is not affine");
  WaterMatrix inv{};auto& a=world;
  double determinant=a[0][0]*(a[1][1]*a[2][2]-a[1][2]*a[2][1])-a[0][1]*(a[1][0]*a[2][2]-a[1][2]*a[2][0])+a[0][2]*(a[1][0]*a[2][1]-a[1][1]*a[2][0]);
  if(!std::isfinite(determinant)||std::abs(determinant)<1e-10)throw std::runtime_error("Physical water singular world matrix");
  for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c){
    unsigned r1=(c+1)%3,r2=(c+2)%3,c1=(r+1)%3,c2=(r+2)%3;
    inv[r][c]=(a[r1][c1]*a[r2][c2]-a[r1][c2]*a[r2][c1])/determinant;
  }
  inv[3][3]=1;for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)inv[r][3]-=inv[r][c]*a[c][3];
  auto vp=waterPackedMatrix(d,0);WaterMatrix camera{};
  for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)for(unsigned k=0;k<4;++k)camera[r][c]+=vp[r][k]*inv[k][c];
  auto projection=traceMatrixProjection(camera);
  if(!d.debugProjection.valid||!traceProjectionMatches(projection,d.debugProjection.values))throw std::runtime_error("Physical water camera projection mismatch");
  std::array<float,3> n={float(camera[0][1]/projection[0]),float(camera[1][1]/projection[1]),float(-camera[3][1])};
  double length=0;for(auto v:n)length+=double(v)*v;
  if(!std::isfinite(length)||length<.5||length>2)throw std::runtime_error("Physical water view basis invalid");
  for(auto& v:n)v/=float(std::sqrt(length));return n;
}
class PhysicalWaterRegion {
public:
  using Point=std::array<float,3>;using Triangle=std::array<Point,3>;
private:
  std::vector<Triangle> triangles;
  std::unordered_map<uint64_t,std::vector<uint32_t>> cells;
  static int32_t cell(float x){if(!std::isfinite(x)||std::abs(x)>1e6f)throw std::runtime_error("Physical water coordinate invalid");return int32_t(std::floor(x/4));}
  static uint64_t key(int32_t x,int32_t z){return uint64_t(uint32_t(x))<<32|uint32_t(z);}
public:
  void set(std::vector<Triangle> input){
    if(input.size()>250000)throw std::runtime_error("Physical water triangle limit");
    std::unordered_map<uint64_t,std::vector<uint32_t>> next;size_t references=0;
    for(uint32_t i=0;i<input.size();++i){auto& t=input[i];for(auto p:t)for(float v:p)if(!std::isfinite(v)||std::abs(v)>1e6f)throw std::runtime_error("Physical water vertex invalid");
      auto minX=cell(std::min({t[0][0],t[1][0],t[2][0]})),maxX=cell(std::max({t[0][0],t[1][0],t[2][0]}));
      auto minZ=cell(std::min({t[0][2],t[1][2],t[2][2]})),maxZ=cell(std::max({t[0][2],t[1][2],t[2][2]}));
      uint64_t count=uint64_t(maxX-minX+1)*uint64_t(maxZ-minZ+1);
      if(count>4096||references+count>2000000)throw std::runtime_error("Physical water spatial index budget");references+=count;
      for(int32_t x=minX;x<=maxX;++x)for(int32_t z=minZ;z<=maxZ;++z)next[key(x,z)].push_back(i);
    }triangles=std::move(input);cells=std::move(next);
  }
  bool empty()const{return triangles.empty();}
  bool contains(const Point& p)const{
    auto found=cells.find(key(cell(p[0]),cell(p[2])));if(found==cells.end())return false;
    for(auto i:found->second){auto& t=triangles[i];double ax=t[1][0]-t[0][0],az=t[1][2]-t[0][2],bx=t[2][0]-t[0][0],bz=t[2][2]-t[0][2];
      double det=ax*bz-az*bx;if(std::abs(det)<1e-10)continue;
      double px=p[0]-t[0][0],pz=p[2]-t[0][2],u=(px*bz-pz*bx)/det,v=(ax*pz-az*px)/det;
      if(u<-.002||v<-.002||u+v>1.002)continue;
      double y=t[0][1]+u*(t[1][1]-t[0][1])+v*(t[2][1]-t[0][1]);
      if(std::abs(y-p[1])<=.15)return true;
    }return false;
  }
  std::vector<uint32_t> filter(const TraceMesh& mesh,const WaterMatrix& world)const{
    if(mesh.positions.empty()||mesh.positions.size()>16384||mesh.indices.empty()||mesh.indices.size()>100000||mesh.indices.size()%3)return {};
    std::vector<Point> points;points.reserve(mesh.positions.size());
    for(auto p:mesh.positions){Point q{};for(unsigned r=0;r<3;++r){double v=world[r][3];for(unsigned k=0;k<3;++k)v+=world[r][k]*p[k];q[r]=float(v);
      if(!std::isfinite(q[r])||std::abs(q[r])>1e6f)return {}; }points.push_back(q);}
    std::vector<uint32_t> result;
    for(size_t i=0;i<mesh.indices.size();i+=3){auto a=points.at(mesh.indices[i]),b=points.at(mesh.indices[i+1]),c=points.at(mesh.indices[i+2]);
      double x1=b[0]-a[0],y1=b[1]-a[1],z1=b[2]-a[2],x2=c[0]-a[0],y2=c[1]-a[1],z2=c[2]-a[2];
      double x=y1*z2-z1*y2,y=z1*x2-x1*z2,z=x1*y2-y1*x2,length=x*x+y*y+z*z;
      if(length<1e-12||y*y<.95*.95*length||!contains(a)||!contains(b)||!contains(c))continue;
      bool ok=contains({(a[0]+b[0]+c[0])/3,(a[1]+b[1]+c[1])/3,(a[2]+b[2]+c[2])/3});
      for(auto edge:{std::array<Point,2>{a,b},{b,c},{c,a}})
        ok=ok&&contains({(edge[0][0]+edge[1][0])*.5f,(edge[0][1]+edge[1][1])*.5f,(edge[0][2]+edge[1][2])*.5f});
      if(ok)result.insert(result.end(),mesh.indices.begin()+i,mesh.indices.begin()+i+3);
    }return result;
  }
  bool matches(const TraceMesh& mesh,const WaterMatrix& world)const{return !mesh.indices.empty()&&filter(mesh,world).size()==mesh.indices.size();}

};
// Reproduce the raw mesh normalizer's first-seen vertex order, preserving
// every original attribute stream for the filtered private replay.
inline std::string physicalWaterNormalSource(std::string source,bool terrain=false){
  auto ret=source.rfind("return output;");
  bool shape=terrain?(source.find("r0 = t0 * t1;")!=std::string::npos&&source.find("r0.xyz = clamp((v0.xyz * r0.www) + r0.xyz")!=std::string::npos):
      (source.find("r0.xyz = mix(r0.xyz, t1.xyz, r0.www);")!=std::string::npos&&source.find("r1.w = t0.w * t1.w;")!=std::string::npos);
  if(ret==std::string::npos||!shape)throw std::runtime_error("Physical water shader family shape changed");
  std::string coverage=terrain?"clamp(t0.a*t1.a,0.0,1.0)":"clamp(t0.a,0.0,1.0)";
  source.insert(ret,"float fo2coverage="+coverage+";if(fo2coverage<=1.0/255.0)discard_fragment();\noutput.oC0=float4(float3(fo2fx[48],fo2fx[49],fo2fx[50])*.5+.5,fo2coverage);\n");return source;
}
}
