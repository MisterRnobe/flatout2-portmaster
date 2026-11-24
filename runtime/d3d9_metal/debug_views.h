#pragma once
namespace fo2render {
inline const char *debugViewSource=R"MSL(
#include <metal_stdlib>
using namespace metal;
struct DebugVertex { float4 position [[position]]; };
vertex DebugVertex debugVS(uint i [[vertex_id]]) {
    return {float4(i==1?3.0:-1.0,i==2?3.0:-1.0,0,1)};
}
float3 debugPosition(uint2 p,depth2d<float,access::read> image,constant float4* c) {
    float distance=c[0].w/(image.read(p)-c[0].z);
    float2 uv=(float2(p)+.5)/float2(image.get_width(),image.get_height());
    float2 ndc=float2(uv.x*2-1,1-uv.y*2)-c[1].zw;
    return float3(ndc*distance/c[0].xy,-distance);
}
fragment float4 debugPS(float4 pixel [[position]],depth2d<float,access::read> image [[texture(0)]],
                        constant float4* c [[buffer(0)]]) {
    uint2 p=min(uint2(pixel.xy),uint2(image.get_width()-1,image.get_height()-1));
    float depth=image.read(p);
    if(!isfinite(depth)||depth>=.9999999)return float4(0,0,0,1);
    float3 center=debugPosition(p,image,c);
    if(c[1].y<.5)return float4(float3(1-clamp(-center.z/c[1].x,0.0,1.0)),1);
    uint2 l=uint2(p.x?p.x-1:0,p.y),r=uint2(min(p.x+1,image.get_width()-1),p.y);
    uint2 u=uint2(p.x,p.y?p.y-1:0),d=uint2(p.x,min(p.y+1,image.get_height()-1));
    float3 left=center-debugPosition(l,image,c),right=debugPosition(r,image,c)-center;
    float3 up=center-debugPosition(u,image,c),down=debugPosition(d,image,c)-center;
    float3 dx=p.x==0?right:(p.x+1==image.get_width()?left:(abs(left.z)<abs(right.z)?left:right));
    float3 dy=p.y==0?down:(p.y+1==image.get_height()?up:(abs(up.z)<abs(down.z)?up:down));
    float3 normal=cross(dx,dy);
    float length2=dot(normal,normal);
    if(!isfinite(length2)||length2<1e-20)return float4(1,0,1,1);
    normal*=rsqrt(length2);
    if(dot(normal,center)>0)normal=-normal;
    return float4(normal*.5+.5,1);
}
)MSL";
} // namespace fo2render
