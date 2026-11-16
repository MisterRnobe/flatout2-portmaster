#pragma once
#include <array>
#include <vector>
#include <memory>
#include <mutex>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include "audio_resampler.h"
struct PcmVoice {
    std::vector<uint8_t> samples;
    unsigned channels=2,bits=16,rate=48000,frequency=48000;
    double position=0;
    float left=1,right=1;
    bool playing=false,loop=false;
    unsigned block()const{return channels*(bits/8);}
    // Streaming rings (the game rewrites parts with Lock/Unlock): one flag per 64 frames, set when the guest
    // publishes it, cleared when the mixer enters it. Entering a cleared one = a stretch played again without
    // new data (the game's mixer fell behind the cursor: repeats, crackles). Counted in staleGranules.
    static constexpr unsigned granule=64;
    std::vector<uint8_t> fresh;
    uint64_t staleGranules=0,playedGranules=0;
    // Streaming FIFO (FO2_AUDIO_FIFO=0 turns it off). FMOD mixes everything into one 93 ms ring; when the guest's
    // mixer thread is late (long frames on small chips: it only runs when the game thread lets the guest machine go)
    // the output played the ring's old contents again: music and sounds stuttered (GitHub issue 1, R50S). The blocks
    // the guest publishes while the ring plays are queued here instead and played in order. When the queue runs dry
    // the voice is silent until it holds fifoTarget frames again, and fifoTarget grows to the length of the dry
    // spell (spells up to 150 ms; longer ones are loading screens): the latency adapts to the console, and stays what it
    // was where the guest keeps up.
    // The play cursor the guest reads (position) keeps moving with time either way.
    std::vector<uint8_t> fifo;
    size_t fifoRead=0,fifoAvail=0,fifoTarget=0,fifoDry=0;   // byte index, frames, frames, source frames
    double fifoFrac=0;
    bool fifoActive=false,fifoWaiting=true,fifoStarted=false,fifoDryFrozen=false;   // fifoDryFrozen: new data came, the dry spell is measured
    uint64_t fifoUnderruns=0;
    size_t fifoWinMin=~size_t(0),fifoWinFrames=0;bool fifoFast=false;
    static bool fifoEnabled(){static const bool on=[]{const char* e=std::getenv("FO2_AUDIO_FIFO");return !(e&&e[0]=='0');}();return on;}
    // FO2_AUDIO_DELAY_MS: the queue's starting depth (default 20 ms; it only grows from there)
    static unsigned fifoStartMs(){static const unsigned ms=[]{const char* e=std::getenv("FO2_AUDIO_DELAY_MS");const int n=e?std::atoi(e):20;return unsigned(n<0?0:n>250?250:n);}();return ms;}
    void fifoReset(){fifoActive=false;fifoRead=fifoAvail=fifoDry=0;fifoFrac=0;fifoWaiting=true;fifoStarted=false;fifoDryFrozen=false;fifoFast=false;fifoWinMin=~size_t(0);fifoWinFrames=0;}
    void fifoDrop(size_t frames){frames=std::min(frames,fifoAvail);fifoRead=(fifoRead+frames*block())%fifo.size();fifoAvail-=frames;}
    void fifoPush(const uint8_t* data,size_t bytes){
        const size_t blk=block(),frames=bytes/blk;if(!frames)return;
        if(fifo.empty()){fifo.assign(size_t(65536)*blk,0);fifoTarget=size_t(frequency)*fifoStartMs()/1000;}
        const size_t capacity=fifo.size()/blk;
        if(frames>=capacity)return;
        if(fifoAvail+frames>capacity)fifoDrop(fifoAvail+frames-capacity);
        size_t at=(fifoRead+fifoAvail*blk)%fifo.size();
        const size_t first=std::min(frames*blk,fifo.size()-at);
        std::memcpy(fifo.data()+at,data,first);if(first<frames*blk)std::memcpy(fifo.data(),data+first,frames*blk-first);
        fifoAvail+=frames;fifoActive=true;if(fifoWaiting)fifoDryFrozen=true;
        // never more than the target plus two rings behind: the guest caught up after a lost lap, or the output stalled
        const size_t ring=samples.size()/blk;
        if(fifoAvail>fifoTarget+2*ring)fifoDrop(fifoAvail-(fifoTarget+ring));
    }
    float fifoSample(size_t frame,unsigned channel)const{
        const size_t offset=(fifoRead+frame*block()+std::min(channel,channels-1)*(bits/8))%fifo.size();
        if(bits==8)return (int(fifo[offset])-128)/128.f;
        const uint16_t word=uint16_t(fifo[offset])|(uint16_t(fifo[(offset+1)%fifo.size()])<<8);
        return static_cast<int16_t>(word)/32768.f;
    }
    // one output frame; step = source frames per output frame
    void fifoMix(float* out,double step){
        if(fifoWaiting){
            if(fifoAvail>=std::max<size_t>(fifoTarget,2)){
                // a spell over 150 ms is a loading screen or a pause, not a late frame: it does not count
                if(fifoStarted&&fifoDry<=size_t(frequency)*3/20)fifoTarget=std::max(fifoTarget,fifoDry+256);
                fifoWaiting=false;fifoStarted=true;fifoDry=0;
            }else{if(!fifoDryFrozen)fifoDry+=size_t(step+0.5);return;}
        }
        if(fifoAvail<2){fifoWaiting=true;fifoDry=0;fifoDryFrozen=false;++fifoUnderruns;fifoFast=false;fifoWinMin=~size_t(0);fifoWinFrames=0;return;}
        // Back to a short queue when the cushion was not needed: over each 30 s without a dry spell the lowest level is
        // kept; if more than the starting depth plus 10 ms was always left, the next window plays 1% faster until the
        // queue is down to that (menus and loading screens leave it deep, a race on a fast console does not need it).
        {
            const size_t floorFrames=size_t(frequency)*(fifoStartMs()+10)/1000;
            fifoWinMin=std::min(fifoWinMin,fifoAvail);
            if(++fifoWinFrames>=size_t(frequency)*30){
                fifoFast=fifoWinMin!=~size_t(0)&&fifoWinMin>floorFrames;
                if(fifoFast)fifoTarget=size_t(frequency)*fifoStartMs()/1000;
                fifoWinMin=~size_t(0);fifoWinFrames=0;
            }
            if(fifoFast){if(fifoAvail>floorFrames)step*=1.01;else fifoFast=false;}
        }
        const float f=float(fifoFrac);
        for(unsigned c=0;c<2;++c){const float a=fifoSample(0,c),b=fifoSample(1,c);out[c]+=(a+(b-a)*f)*(c?right:left);}
        fifoFrac+=step;
        while(fifoFrac>=1&&fifoAvail){fifoFrac-=1;fifoDrop(1);}
    }
    void markWritten(size_t firstByte,size_t bytes){
        if(!bytes)return;
        if(fresh.empty())fresh.assign(samples.size()/block()/granule+1,0);
        const size_t a=firstByte/block()/granule,b=(firstByte+bytes-1)/block()/granule;
        for(size_t g=a;g<=b&&g<fresh.size();++g)fresh[g]=1;
    }
};
class PcmMixer {
public:
    PcmMixer(){(void)nfs_audio::upsampleKernel();} // No filter construction in the real-time callback.
    std::mutex mutex;
    // Every DirectSound buffer holds a slot from its creation: Alien Shooter 2 loads 225+ sounds at once
    // (128 slots ran out: DSERR_ALLOCATED for the rest and for the music).
    std::array<std::shared_ptr<PcmVoice>,1024> voices{};
    unsigned rate=48000;
    // Protected by mutex, including callback updates. Inspect before clamping.
    uint64_t clippedSamples=0;float peakAmplitude=0;
    bool add(const std::shared_ptr<PcmVoice>& voice){
        std::lock_guard<std::mutex> lock(mutex);
        for(auto& slot:voices)if(!slot){slot=voice;return true;}return false;
    }
    void remove(const std::shared_ptr<PcmVoice>& voice){std::lock_guard<std::mutex> lock(mutex);for(auto& slot:voices)if(slot==voice)slot.reset();}
    static float sample(const PcmVoice& voice,size_t frame,unsigned channel){
        size_t offset=frame*voice.block()+std::min(channel,voice.channels-1)*(voice.bits/8);
        if(voice.bits==8)return (int(voice.samples[offset])-128)/128.f;
        uint16_t word=uint16_t(voice.samples[offset])|(uint16_t(voice.samples[offset+1])<<8);
        return static_cast<int16_t>(word)/32768.f;
    }
    void render(float* output,unsigned frames){
        std::fill(output,output+frames*2,0.f);
        const auto& kernel=nfs_audio::upsampleKernel();
        // The guest holds this lock only for short copies of the regions it
        // unlocked and for cursor queries, so waiting is bounded. Skipping a
        // burst instead output silence and left every play cursor where it
        // was, so the game's view of what had played drifted from what had.
        std::lock_guard<std::mutex> lock(mutex);
        for(auto& voice:voices){
            if(!voice||!voice->playing||voice->samples.empty())continue;
            size_t count=voice->samples.size()/voice->block();if(!count)continue;
            double step=double(voice->frequency)/rate;
            size_t lastGranule=size_t(voice->position)/PcmVoice::granule;
            for(unsigned frame=0;frame<frames;++frame){
                if(voice->position>=count){if(voice->loop)voice->position=std::fmod(voice->position,double(count));else{voice->playing=false;voice->position=0;break;}}
                if(voice->fifoActive){voice->fifoMix(output+frame*2,step);voice->position+=step;continue;}
                size_t a=size_t(voice->position),b=a+1;float fraction=float(voice->position-a);
                if(!voice->fresh.empty()&&a/PcmVoice::granule!=lastGranule){
                    lastGranule=a/PcmVoice::granule;
                    if(lastGranule<voice->fresh.size()){++voice->playedGranules;if(!voice->fresh[lastGranule])++voice->staleGranules;voice->fresh[lastGranule]=0;}
                }
                if(b==count)b=voice->loop?0:a;
                if(step<1){
                    float phase=fraction*nfs_audio::UpsampleKernel::phases;
                    unsigned index=std::min(unsigned(phase),nfs_audio::UpsampleKernel::phases-1);
                    float blend=phase-index,left=0,right=0;
                    for(unsigned tap=0;tap<nfs_audio::UpsampleKernel::taps;++tap){
                        int64_t source=int64_t(a)+int(tap)-7;
                        if(source<0||uint64_t(source)>=count){
                            if(voice->loop){source%=int64_t(count);if(source<0)source+=count;}
                            else source=std::clamp(source,int64_t(0),int64_t(count)-1);
                        }
                        float weight=kernel.weights[index][tap]+(kernel.weights[index+1][tap]-kernel.weights[index][tap])*blend;
                        left+=sample(*voice,size_t(source),0)*weight;right+=sample(*voice,size_t(source),1)*weight;
                    }
                    output[frame*2]+=left*voice->left;output[frame*2+1]+=right*voice->right;
                }else for(unsigned channel=0;channel<2;++channel){float first=sample(*voice,a,channel),second=sample(*voice,b,channel);
                    output[frame*2+channel]+=(first+(second-first)*fraction)*(channel?voice->right:voice->left);}
                voice->position+=step;
            }
        }
        for(unsigned i=0;i<frames*2;++i){
            float amplitude=std::abs(output[i]);peakAmplitude=std::max(peakAmplitude,amplitude);
            if(amplitude>1.f)++clippedSamples;
            output[i]=std::clamp(output[i],-1.f,1.f);
        }
    }
};
