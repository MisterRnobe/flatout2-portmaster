#include "../runtime/render/trace_update_worker.h"
#include <cassert>
#include <future>
#include <cstdio>
using namespace fo2render;
int main(){
  std::promise<void> started,release,done,reset;auto unlock=release.get_future().share();
  std::atomic<unsigned> count{0};std::atomic<bool> stale{false};
  TraceUpdateWorker worker([&](const auto& s,const auto& current){assert(s.frame==10);++count;started.set_value();unlock.wait();stale=!current();done.set_value();},[&]{reset.set_value();});
  LiveTraceSnapshot first;first.frame=10;assert(worker.canAccept());assert(worker.submit(std::move(first)));started.get_future().wait();
  LiveTraceSnapshot another;another.frame=11;assert(!worker.canAccept());assert(!worker.submit(std::move(another)));
  worker.invalidate();release.set_value();done.get_future().wait();reset.get_future().wait();
  assert(stale&&count==1);worker.stop();assert(!worker.canAccept());assert(!worker.submit(std::move(another)));
  std::promise<void> errored;TraceUpdateWorker failed([](const auto&,const auto&){throw std::runtime_error("intentional CPU test error");},[&]{errored.set_value();});
  LiveTraceSnapshot bad;assert(failed.submit(std::move(bad)));errored.get_future().wait();assert(failed.failed());failed.stop();
  // Repeated frames run in order after each completed job; stop joins safely.
  std::atomic<uint64_t> last{0};
  TraceUpdateWorker repeated([&](const auto& s,const auto& current){assert(current());assert(s.frame>last);last=s.frame;},[]{});
  for(uint64_t i=1;i<=100;++i){while(!repeated.canAccept())std::this_thread::yield();LiveTraceSnapshot s;s.frame=i;assert(repeated.submit(std::move(s)));while(!repeated.canAccept())std::this_thread::yield();}
  assert(last==100);repeated.stop();
  std::puts("PASS: single-flight ownership/backpressure, stale generation rejection, invalidate, failure isolation, repeated updates and joined shutdown");
}
