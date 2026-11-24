#pragma once
#include "live_trace_scene.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <cstdio>
namespace fo2render {
// Single flight: one owned scene, no accumulating queue or old-frame backlog.
class TraceUpdateWorker {
public:
  using Current=std::function<bool()>;
  using Work=std::function<void(const LiveTraceSnapshot&,const Current&)>;
private:
  mutable std::mutex mutex;std::condition_variable ready;std::optional<LiveTraceSnapshot> pending;
  Work work;std::function<void()> clear;
  bool stopping=false,active=false,resetPending=false;std::atomic<bool> failedFlag{false};
  std::atomic<uint64_t> generation{0};std::thread thread;
  void run(){
    for(;;){std::unique_lock<std::mutex> lock(mutex);ready.wait(lock,[&]{return stopping||resetPending||pending.has_value();});
      if(stopping)return;
      if(resetPending){resetPending=false;lock.unlock();try{clear();}catch(...){failedFlag=true;}continue;}
      auto job=std::move(*pending);pending.reset();active=true;auto epoch=generation.load();lock.unlock();
      try{work(job,[this,epoch]{return generation.load()==epoch;});}
      catch(const std::exception& e){failedFlag=true;std::fprintf(stderr,"[FO2 RT UPDATE] Disabled after error: %s; gameplay remains SSR\n",e.what());try{clear();}catch(...){}}
      catch(...){failedFlag=true;try{clear();}catch(...){}}
      lock.lock();active=false;ready.notify_all();
    }
  }
public:
  TraceUpdateWorker(Work callback,std::function<void()> reset):work(std::move(callback)),clear(std::move(reset)),thread([this]{run();}){}
  ~TraceUpdateWorker(){stop();}
  void stop(){ {std::lock_guard<std::mutex> lock(mutex);stopping=true;++generation;pending.reset();}ready.notify_all();if(thread.joinable())thread.join();}
  TraceUpdateWorker(const TraceUpdateWorker&)=delete;
  bool failed()const{return failedFlag.load();}
  bool canAccept()const{std::lock_guard<std::mutex> lock(mutex);return !stopping&&!active&&!pending&&!resetPending&&!failed();}
  bool submit(LiveTraceSnapshot&& scene){std::lock_guard<std::mutex> lock(mutex);
    if(stopping||active||pending||resetPending||failed())return false;
    pending=std::move(scene);ready.notify_one();return true;}
  void invalidate(){std::lock_guard<std::mutex> lock(mutex);++generation;pending.reset();resetPending=true;ready.notify_one();}
};
}
