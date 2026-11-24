#include "../runtime/d3d9_metal/physical_water_bridge.h"
#include <cassert>
using namespace fo2render;
int main(int argc,char** argv){@autoreleasepool{
  assert(argc==2);setenv("FO2_WATER_PHYSICS_SIGNATURES",argv[1],1);
  auto file=std::filesystem::path(argv[1]).parent_path()/"collision-1203.json";
  auto data=[NSData dataWithContentsOfFile:[NSString stringWithUTF8String:file.c_str()]];assert(data);
  id doc=[NSJSONSerialization JSONObjectWithData:data options:0 error:nullptr];assert(doc);
  std::vector<float> positions;
  for(id t in doc[@"water_triangles"]){auto points=t[@"points"];float p[3][3];for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)p[r][c]=[points[r][c] floatValue];
    double a[3],b[3],n[3],length=0;for(unsigned k=0;k<3;++k){a[k]=p[1][k]-p[0][k];b[k]=p[2][k]-p[0][k];}
    for(unsigned k=0;k<3;++k){n[k]=a[(k+1)%3]*b[(k+2)%3]-a[(k+2)%3]*b[(k+1)%3];length+=n[k]*n[k];}
    if(length>1e-4&&n[1]*n[1]>.95*.95*length){for(auto& point:p)for(float f:point)positions.push_back(f);break;}
  }assert(positions.size()==9);
  Draw d;d.captureMaterial=true;d.physicalWaterAsset=1204;d.depth=std::make_shared<Image>();d.raster.depthOn=true;
  d.vs=std::make_shared<Shader>();d.ps=std::make_shared<Shader>();d.vs->physicalWaterFamily=d.ps->physicalWaterFamily=2;d.ps->terrainSpecularMask=true;
  d.vs->inputs={{0,0,0}};d.attributes={{0,0,0,Element::Float3}};d.strides[0]=12;d.count=3;d.primitive=Primitive::Triangles;
  d.streams[0]=std::make_shared<Buffer>();d.streams[0]->bytes.resize(36);std::memcpy(d.streams[0]->bytes.data(),positions.data(),36);
  float constants[]={1,0,0,0,0,1,0,-10,0,0,1.01f,100.9f,0,0,1,100,0,10,-100,1,0,0,1,0};
  d.vs->uniforms={{0,0,4},{0,8,2}};d.vsConstants.resize(sizeof constants);std::memcpy(d.vsConstants.data(),constants,sizeof constants);
  d.debugProjection={{1,1,1.01f,-.1f},true};PhysicalWaterBridge bridge(true);std::array<float,3> normal{};std::shared_ptr<Buffer> indices;
  assert(bridge.match(d,normal,indices)&&indices&&indices->bytes.size()==12&&std::abs(normal[1]-1)<1e-5);
  auto first=indices;indices.reset();assert(bridge.match(d,normal,indices)&&indices==first);
  auto dry=positions;for(unsigned i=0;i<3;++i)dry[i*3+1]+=2;
  std::memcpy(d.streams[0]->bytes.data(),dry.data(),36);++d.streams[0]->version;indices.reset();assert(!bridge.match(d,normal,indices));
  positions.insert(positions.end(),dry.begin(),dry.end());d.count=6;d.streams[0]->bytes.resize(72);std::memcpy(d.streams[0]->bytes.data(),positions.data(),72);++d.streams[0]->version;
  indices.reset();assert(bridge.match(d,normal,indices)&&indices->bytes.size()==12); // mixed batch filters only wet triangle.
  d.physicalWaterAsset=5034;indices.reset();assert(!bridge.match(d,normal,indices)); // pool has separate WorldWater, no physical-water triangles.
  std::puts("PASS: real derby catalog load, physics-backed triangle replay, cache/version invalidation, mixed dry/wet batch and level change; no GPU used");
}}
