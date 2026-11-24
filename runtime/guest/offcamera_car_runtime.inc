// Included after callGuest/State are defined. No guest memory patch or stale
// mesh replay: new nodes enter the original batch builder with their current
// transforms, model selection, deformed buffers and material constants.
extern "C" {
int fo2_rt_scene_enabled=[]{
#ifdef FO2_METAL
    const char* water=std::getenv("FO2_RT_WATER"),*scene=std::getenv("FO2_RT_CAR_SCENE");
    return water&&std::strcmp(water,"1")==0&&(!scene||std::strcmp(scene,"0")!=0)?1:0;
#else
    return 0;
#endif
}();
}
struct OffcameraFrame {
    uint64_t frame=0,batchEntries=0;uint32_t context=0,camera=0,buckets=0;
    bool submitted=false,busy=false,disabled=false,rayOnlyAtEntry=false;
    fo2offcamera::Provenance provenance;
};
static thread_local OffcameraFrame offcameraFrame;
extern "C" int fo2OffcameraRayOnlyDraw(){return offcameraFrame.provenance.rayOnly()?1:0;}
extern "C" void fo2_rt_scene_leave(uint32_t){offcameraFrame.provenance.leave();}
extern "C" void fo2_rt_scene_enter(uint32_t va){
    auto& f=offcameraFrame;
    if(va==0x004C9DC0u){++f.frame;f.context=f.camera=f.buckets=0;f.submitted=false;f.provenance.reset();return;}
    if(va==0x005A6EF0u){f.provenance.setWorld(0);return;}
    auto read=[](uint32_t address,void* destination,size_t size){if(!memory||!memory->readable(address,size))return false;
        std::memcpy(destination,ptr(address,size),size);return true;};
    auto arg=[&](uint32_t offset){uint32_t value=0;if(g_esp<=UINT32_MAX-offset)read(g_esp+offset,&value,4);return value;};
    if(va==0x005A6F20u){f.provenance.setWorld(arg(4));return;}
    if(va==0x00553CA0u){f.provenance.enter(f.context&&f.buckets&&arg(4)==f.buckets);++f.batchEntries;f.rayOnlyAtEntry=f.provenance.rayOnly();}
    if(!fo2InRace()){f.provenance.reset();f.context=f.camera=f.buckets=0;return;}
    if(f.disabled||f.busy)return;
    if(va==0x004C9B80u){
        f.context=arg(4);f.camera=arg(8);f.buckets=0;f.submitted=false;
        uint32_t views=0;if(arg(12)!=0||f.context>UINT32_MAX-0x1bc||!read(f.context+0x1bc,&views,4)||views!=1)f.context=f.camera=0;
        if(f.context)read(f.context+0x1b4,&f.buckets,4); // View zero's bucket array (0x4CA1DC).
        return;
    }
    if(!f.context||!f.camera)return;
    if(va!=0x00553CA0u||f.submitted||!f.buckets||arg(4)!=f.buckets)return;
    f.submitted=true;f.busy=true;State saved;saved.save();auto started=std::chrono::steady_clock::now();
    try{
        auto selection=fo2offcamera::select(f.context,f.camera,read,f.buckets);
        for(auto node:selection.nodes){f.provenance.add(node.world);g_ecx=node.address;
            callGuest(0x0054CD20u,{f.camera,0,f.buckets});}
        {
            static thread_local unsigned early=0;
            if(f.frame%300==0||early++<3){
                double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
                std::printf("[FO2 RT POPULATION] frame=%llu cars=%u visited=%u visible=%u ray-only-nodes=%zu hidden=%u distant=%u unsupported=%u prepare=%.3f ms\n",
                    (unsigned long long)f.frame,selection.cars,selection.visited,selection.visible,selection.nodes.size(),selection.hidden,selection.distant,selection.unsupported,ms);
            }
        }
    }catch(const std::exception& e){f.disabled=true;
        // Keep provenance for any already-enqueued nodes until this frame ends.
        std::fprintf(stderr,"[FO2 RT POPULATION] Further collection disabled: %s; queued ray-only provenance retained\n",e.what());}
    saved.load();f.busy=false;
}
static void testOffcameraLiftedScope(){
#ifdef FO2_METAL
    auto savedFrame=offcameraFrame;State saved;saved.save();int enabled=fo2_rt_scene_enabled;
    auto physics=lastRacePhysics.load(std::memory_order_relaxed);uint32_t arena=allocate(0x700),bucket=arena+0x500;
    auto restore=[&]{saved.load();offcameraFrame=std::move(savedFrame);fo2_rt_scene_enabled=enabled;
        lastRacePhysics.store(physics,std::memory_order_relaxed);heaps->free(GuestHeaps::process,arena);};
    try{
        std::memset(ptr(arena,0x700),0,0x700);offcameraFrame={};fo2_rt_scene_enabled=1;
        lastRacePhysics.store(std::chrono::steady_clock::now().time_since_epoch().count(),std::memory_order_relaxed);
        if(!fo2InRace())throw GuestStop("Offcamera fixture expected an unpaused physics marker");
        uint32_t node=arena,cam=arena+0x140,model=arena+0x280,batch=arena+0x2d0,list=arena+0x310,items=arena+0x320,buckets=arena+0x380;
        float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        std::memcpy(ptr(node+0x80,64),identity,64);std::memcpy(ptr(cam+0x80,64),identity,64);float z=4,one=1;
        std::memcpy(ptr(node+0xb8,4),&z,4);std::memcpy(ptr(cam+0x11c,4),&one,4);
        write32(node,0x67ae30);write32(node+0xf0,model);write32(node+0x108,1);write32(model+0x30,list);write32(model+0x34,list+4);write32(list,batch);
        write32(buckets,items);write32(buckets+4,items+16);write32(buckets+8,items+64);write32(items,0x12345678);
        g_ecx=node;callGuest(0x54cd20,{cam,0,buckets});
        if(read32(items)!=0x12345678||read32(buckets+4)!=items+32||read32(items+16)!=batch||read32(items+20)!=node+0x80)
            throw GuestStop("Offcamera lifted mesh did not append its current batch/world after ordinary entries");
        write32(node+0x10,2);callGuest(0x54cd20,{cam,0,buckets});
        if(read32(buckets+4)!=items+32)throw GuestStop("Offcamera lifted mesh ignored authored hidden flag");
        g_ecx=saved.ecx;
        offcameraFrame.context=arena;offcameraFrame.camera=cam;offcameraFrame.buckets=bucket;offcameraFrame.submitted=true;
        offcameraFrame.provenance.add(0x123400);offcameraFrame.provenance.setWorld(0x123400);
        callGuest(0x00553CA0u,{bucket,0});
        if(offcameraFrame.batchEntries!=1||!offcameraFrame.rayOnlyAtEntry||fo2OffcameraRayOnlyDraw())
            throw GuestStop("Lifted offcamera entry/return scope failed (full guest rebuild required)");
        State after;after.save();
        if(after.eax!=saved.eax||after.ecx!=saved.ecx||after.esp!=saved.esp||after.ebp!=saved.ebp||after.ebx!=saved.ebx||after.esi!=saved.esi||after.edi!=saved.edi||
           std::memcmp(after.flags,saved.flags,sizeof saved.flags)||std::memcmp(after.xmm,saved.xmm,sizeof saved.xmm)||std::memcmp(after.st,saved.st,sizeof saved.st))
            throw GuestStop("Offcamera lifted callbacks changed caller CPU state");
        restore();std::puts("[FO2 RT POPULATION CHECK] actual lifted mesh append, ordinary-entry priority, hidden flags, guest CPU/stack restoration and ray-only return cleanup passed");
    }catch(...){restore();throw;}
#endif
}
