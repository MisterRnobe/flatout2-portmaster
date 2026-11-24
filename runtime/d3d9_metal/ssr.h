#pragma once
namespace fo2render {
// Appended to debugViewSource to share depth/projection reconstruction.
inline const char* ssrSource=R"MSL(
float3 ssrNormal(uint2 p,depth2d<float,access::read> image,constant float4* c){
    uint2 size=uint2(image.get_width(),image.get_height());
    float3 center=debugPosition(p,image,c);
    float3 l=center-debugPosition(uint2(p.x?p.x-1:0,p.y),image,c);
    float3 r=debugPosition(uint2(min(p.x+1,size.x-1),p.y),image,c)-center;
    float3 u=center-debugPosition(uint2(p.x,p.y?p.y-1:0),image,c);
    float3 d=debugPosition(uint2(p.x,min(p.y+1,size.y-1)),image,c)-center;
    float3 dx=p.x==0?r:(p.x+1==size.x?l:(abs(l.z)<abs(r.z)?l:r));
    float3 dy=p.y==0?d:(p.y+1==size.y?u:(abs(u.z)<abs(d.z)?u:d));
    float3 n=cross(dx,dy);float length2=dot(n,n);
    if(!isfinite(length2)||length2<1e-20)return float3(0);
    n*=rsqrt(length2);return dot(n,center)>0?-n:n;
}
float2 ssrUV(float3 q,constant float4* c){
    return (q.xy/(-q.z)*c[0].xy+c[1].zw)*float2(.5,-.5)+.5;
}
// Bounded spatial radiance filter, not another ray march. Reject depth
// discontinuities so rough reflections do not bleed across foreground edges.
float3 ssrRoughRadiance(texture2d<float> scene,depth2d<float,access::read> depth,
                        float2 uv,float roughness,constant float4* c){
    constexpr sampler linearClamp(coord::normalized,address::clamp_to_edge,filter::linear);
    float3 sum=scene.sample(linearClamp,uv).rgb;
    float radius=8*roughness*roughness;
    if(radius<.5)return sum;
    uint2 size=uint2(depth.get_width(),depth.get_height());
    uint2 center=min(uint2(clamp(uv,0.0,.999999)*float2(size)),size-1);
    float centerDistance=c[0].w/(depth.read(center)-c[0].z);
    float weight=1;
    const float2 offsets[4]={float2(-1,0),float2(1,0),float2(0,-1),float2(0,1)};
    for(uint i=0;i<4;++i){
        float2 tap=uv+offsets[i]*radius/float2(size);
        if(any(tap<=0)||any(tap>=1))continue;
        uint2 p=min(uint2(tap*float2(size)),size-1);
        float z=depth.read(p),distance=c[0].w/(z-c[0].z);
        if(z>=.9999999||!isfinite(distance)||abs(distance-centerDistance)>.05+.01*centerDistance)continue;
        sum+=scene.sample(linearClamp,tap).rgb*.5;weight+=.5;
    }
    return sum/weight;
}
fragment float4 ssrPS(float4 pixel [[position]],depth2d<float,access::read> depth [[texture(0)]],
                      texture2d<float> scene [[texture(1)]],texture2d<float> materials [[texture(2)]],texture2d<float> waterNormals [[texture(3)]],
                      depth2d<float,access::read> waterDepth [[texture(4)]],constant float4* c [[buffer(0)]]){
    uint2 size=uint2(depth.get_width(),depth.get_height());
    float2 uv=pixel.xy/c[2].xy;
    uint2 p=min(uint2(uv*float2(size)),size-1);
    float4 wave=c[3].x>.5?waterNormals.read(p):float4(0);
    bool water=wave.a>.001&&waterDepth.read(p)<=depth.read(p)+2e-6;
    float z=water?waterDepth.read(p):depth.read(p);
    if(!isfinite(z)||z>=.9999999)return float4(0);
    float3 center=water?debugPosition(p,waterDepth,c):debugPosition(p,depth,c);
    float3 n=water?normalize(wave.rgb*2-1):ssrNormal(p,depth,c);
    if(dot(n,center)>0)n=-n;
    if(dot(n,n)<.5)return float4(0);
    float4 material=water?float4(c[3].y,c[3].z,c[3].w/255.0,1):(c[2].w>.5?materials.read(p):float4(0));
    if(!water && c[2].w>.5 && abs(material.b*255-c[3].w)<.5)material.r=0;
    float receiver=(c[2].w>.5?(material.r>0?1.0:0.0)*clamp(material.a,0.0,1.0)*(1-.7*material.g):smoothstep(.45,.8,n.y))*(1-smoothstep(100.0,150.0,-center.z));
    if(c[2].z>.5){
        if(water)return float4(0,.6,.8,1);float mask=c[2].w>.5?material.r:receiver;return float4(mask,mask,mask,1);}
    if(water)receiver*=wave.a;
    if(receiver<.001)return float4(0);
    float3 incident=normalize(center),ray=reflect(incident,n);
    float3 origin=center+n*.06;
    float previousT=.25,previousDelta=-1;
    constexpr sampler linearClamp(coord::normalized,address::clamp_to_edge,filter::linear);
    for(uint i=1;i<=48;++i){
        float f=float(i)/48.0,t=.25+60*f*f;
        float3 q=origin+ray*t;
        if(q.z>=-.1)break;
        float2 hitUV=ssrUV(q,c);
        if(any(hitUV<=.001)||any(hitUV>=.999))break;
        uint2 hitPixel=min(uint2(hitUV*float2(size)),size-1);
        float sampleDepth=depth.read(hitPixel);
        // Water is a receiver here; do not reflect an opaque surface hidden by it.
        if(c[3].x>.5&&waterNormals.read(hitPixel).a>.001&&waterDepth.read(hitPixel)<=sampleDepth+2e-6){previousT=t;previousDelta=-1;continue;}
        if(sampleDepth>=.9999999){previousT=t;previousDelta=-1;continue;}
        float distance=c[0].w/(sampleDepth-c[0].z);
        float delta=-q.z-distance;
        if(delta>=0 && previousDelta<0 && delta<.15+.02*distance){
            float lo=previousT,hi=t;
            for(uint j=0;j<5;++j){
                float mid=(lo+hi)*.5;float3 test=origin+ray*mid;
                float2 testUV=ssrUV(test,c);
                uint2 testPixel=min(uint2(clamp(testUV,0.0,.999999)*float2(size)),size-1);
                float testDistance=c[0].w/(depth.read(testPixel)-c[0].z);
                if(-test.z>=testDistance)hi=mid;else lo=mid;
            }
            hitUV=ssrUV(origin+ray*hi,c);
            uint2 refined=min(uint2(clamp(hitUV,0.0,.999999)*float2(size)),size-1);
            if(c[3].x>.5&&waterNormals.read(refined).a>.001&&waterDepth.read(refined)<=depth.read(refined)+2e-6)return float4(0);
            float edge=min(min(hitUV.x,1-hitUV.x),min(hitUV.y,1-hitUV.y));
            float confidence=receiver*smoothstep(0.0,.08,edge)*(1-hi/61.0);
            float grazing=pow(1-clamp(dot(-incident,n),0.0,1.0),5.0);
            // Recover F0 before Fresnel; coverage already attenuates receiver.
            float f0=clamp(material.r/max(material.a,1e-6),0.0,1.0);
            float fresnel=c[2].w>.5?f0+(1-f0)*grazing:.08+.45*grazing;
            return float4(ssrRoughRadiance(scene,depth,hitUV,c[2].w>.5?material.g:0.0,c),confidence*fresnel);
        }
        previousT=t;previousDelta=delta;
    }
    return float4(0);
}
// Reconstruct straight radiance using confidence-premultiplied taps. Ordinary
// RGBA bilinear filtering darkens hit/miss boundaries twice (RGB and alpha).
// Match the receiving surface as well: half-resolution neighbors can belong to
// pool walls, foreground objects or a different reflective material.
float4 ssrResolve(uint2 p,texture2d<float> reflection,texture2d<float> materials,
                  texture2d<float> waterNormals,depth2d<float,access::read> opaqueDepth,
                  depth2d<float,access::read> waterDepth,constant float4* c){
    uint2 size=uint2(opaqueDepth.get_width(),opaqueDepth.get_height());
    uint2 halfSize=uint2(reflection.get_width(),reflection.get_height());
    bool water=c[3].x>.5&&waterNormals.read(p).a>.001&&waterDepth.read(p)<=opaqueDepth.read(p)+2e-6;
    float z=water?waterDepth.read(p):opaqueDepth.read(p);
    if(!isfinite(z)||z>=.9999999)return float4(0);
    float3 center=water?debugPosition(p,waterDepth,c):debugPosition(p,opaqueDepth,c);
    float3 normal=water?normalize(waterNormals.read(p).rgb*2-1):ssrNormal(p,opaqueDepth,c);
    float4 material=c[2].w>.5?materials.read(p):float4(0);
    if(!water&&c[2].w>.5&&(material.r<=0||abs(material.b*255-c[3].w)<.5))return float4(0);
    float2 q=(float2(p)+.5)*float2(halfSize)/float2(size)-.5;
    int2 first=int2(floor(q));float2 f=fract(q);
    float3 radiance=0;float confidence=0,support=0;
    for(uint y=0;y<2;++y)for(uint x=0;x<2;++x){
        uint2 h=uint2(clamp(first+int2(x,y),int2(0),int2(halfSize)-1));
        // This is the same full-resolution receiver selected by ssrPS.
        uint2 tap=min(uint2((float2(h)+.5)*float2(size)/float2(halfSize)),size-1);
        bool tapWater=c[3].x>.5&&waterNormals.read(tap).a>.001&&waterDepth.read(tap)<=opaqueDepth.read(tap)+2e-6;
        if(water!=tapWater)continue;
        if(!water&&c[2].w>.5){float4 m=materials.read(tap);if(m.r<=0||abs(m.b-material.b)>.5/255.0)continue;}
        float tapZ=water?waterDepth.read(tap):opaqueDepth.read(tap);
        if(!isfinite(tapZ)||tapZ>=.9999999)continue;
        float3 position=water?debugPosition(tap,waterDepth,c):debugPosition(tap,opaqueDepth,c);
        // A tangent-plane test accepts sloped receivers but rejects depth steps.
        float tolerance=.025+.005*abs(center.z);
        float weight=(x?f.x:1-f.x)*(y?f.y:1-f.y);
        weight*=1-smoothstep(tolerance,tolerance*2,abs(dot(position-center,normal)));
        if(water)weight*=smoothstep(.75,.95,dot(normal,normalize(waterNormals.read(tap).rgb*2-1)));
        float4 hit=reflection.read(h);
        support+=weight;confidence+=weight*hit.a;radiance+=weight*hit.a*hit.rgb;
    }
    if(confidence<1e-6||support<1e-6)return float4(0);
    return float4(radiance/confidence,confidence/support);
}
// Explicit raster conversion for inspecting RGBA16Float RT outputs. Never
// treat their backing bytes as the renderer's ordinary RGBA8 readback layout.
fragment float4 reflectionCopyPS(float4 pixel [[position]],texture2d<float> source [[texture(0)]]){
    return source.read(uint2(pixel.xy));
}
fragment float4 ssrCompositePS(float4 pixel [[position]],texture2d<float> base [[texture(0)]],
                               texture2d<float> scene [[texture(1)]],texture2d<float> reflection [[texture(2)]],
                               texture2d<float> materials [[texture(3)]],texture2d<float> waterNormals [[texture(4)]],
                               texture2d<float> waterScene [[texture(5)]],depth2d<float,access::read> opaqueDepth [[texture(6)]],
                               depth2d<float,access::read> waterDepth [[texture(7)]],texture2d<float> rtWater [[texture(8)]],constant float4* c [[buffer(0)]]){
    uint2 p=uint2(pixel.xy);float4 original=base.read(p);
    bool water=c[3].x>.5&&waterNormals.read(p).a>.001&&waterDepth.read(p)<=opaqueDepth.read(p)+2e-6;
    float4 hit=ssrResolve(p,reflection,materials,waterNormals,opaqueDepth,waterDepth,c);
    if(water&&c[1].x>.5){
        float4 traced=ssrResolve(p,rtWater,materials,waterNormals,opaqueDepth,waterDepth,c);
        if(traced.a>0)hit=traced;
    }
    if(c[2].z>.5){
        if(water)return float4(0,.6,.8,1);
        if(c[2].w>.5){float4 m=materials.read(p);return float4(float3(abs(m.b*255-c[3].w)<.5?0:m.r),1);}
        return float4(hit.rgb,1);
    }
    // Preserve HUD/overlays where final color differs substantially from scene color.
    float3 reference=water?waterScene.read(p).rgb:scene.read(p).rgb;
    float3 delta=abs(original.rgb-reference);
    float difference=max(max(delta.r,delta.g),delta.b);
    float gain=water&&isfinite(c[1].y)&&c[1].y>=1?clamp(c[1].y,1.0,8.0):1.0;
    float weight=clamp(hit.a*gain,0.0,1.0)*(1-smoothstep(.08,.25,difference));
    if(c[2].w>.5 && !water){float4 m=materials.read(p);if(m.r==0||abs(m.b*255-c[3].w)<.5)weight=0;}
    return float4(mix(original.rgb,hit.rgb,weight),original.a);
}
)MSL";
}
