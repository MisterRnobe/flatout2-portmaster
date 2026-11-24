#include "../runtime/host/test_scenario.h"
#include <cassert>
#include <cstdio>
using namespace fo2test;
int main() {
  RaceScenario race(true);
  assert(race.tick(0,0,false).press == -1);
  assert(race.tick(1,60,false).press == -1);
  for (unsigned i=0;i<8;++i) {
    const double t=2+i*4;
    assert(race.tick(t,120+i*120,false).press == (i==6?1:0));
    auto up=race.tick(t+.3,130+i*120,false);
    assert(up.release==(i==6?1:0) && up.capture);
  }
  assert(race.tick(35,1200,false).press==-1);
  assert(!race.tick(36,1300,true).ready);
  assert(race.tick(36.6,1304,true).ready);
  assert(race.state()==ScenarioState::Ready);
  assert(!race.tick(500,2000,true).failed);
  RaceScenario stalled(true);
  stalled.tick(0,0,false);stalled.tick(1,60,false);
  assert(stalled.tick(2,120,false).press==0);
  assert(stalled.tick(2.3,120,false).release==0);
  assert(stalled.tick(10,120,false).press==-1);
  assert(stalled.tick(180,120,false).failed);
  RaceScenario cancelled(true);
  cancelled.tick(0,0,false);cancelled.tick(1,60,false);cancelled.tick(2,120,false);
  cancelled.cancel();auto stop=cancelled.tick(2.1,120,false);
  assert(stop.cancelled && stop.release==0);
  assert(cancelled.tick(50,1000,false).press==-1);
  RaceScenario early(true);
  early.tick(0,0,false);early.tick(1,60,false);early.tick(2,120,false);
  assert(early.tick(2.1,121,true).release==0);
  assert(early.tick(2.7,125,true).ready);
  RaceScenario debug(true);
  debug.tick(0,1,true,false);
  assert(!debug.tick(1,100,true,false).ready);
  assert(debug.tick(1.1,101,true,true).ready);
  RaceScenario direct(true,true);assert(direct.steps()==5);
  direct.tick(0,0,false);direct.tick(1,60,false);
  for(unsigned i=0;i<4;++i){double t=2+i*4;assert(direct.tick(t,120+i*120,false).press==0);assert(direct.tick(t+.3,130+i*120,false).release==0);}
  // Once Single Player is open, send no fifth confirm into its Career item.
  assert(direct.tick(20,1500,false).press==-1&&direct.step()==4);
  assert(direct.tick(21,1600,false,false,true).press==0&&direct.step()==5);
  assert(direct.tick(21.3,1618,false,false,true).release==0);
  assert(direct.tick(25,1840,false,false,true).press==-1);
  assert(!direct.tick(26,1900,true,false,true).ready);
  assert(direct.tick(27,1960,true,true,true).ready);
  std::puts("Scenario checks passed: finite route, physics-confirmed readiness, frame stalls, timeout, cancellation and input release");
}
