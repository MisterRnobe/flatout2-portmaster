#pragma once
#include <cstdint>
namespace fo2test {
enum class ScenarioState { Waiting, Navigating, AwaitingRace, Ready, Failed, Cancelled };
struct ScenarioUpdate {
  int press = -1, release = -1; // 0: confirm, 1: right
  bool ready = false, failed = false, cancelled = false, capture = false;
};
class RaceScenario {
  bool enabled_;
  bool directRace_;
  ScenarioState state_ = ScenarioState::Waiting;
  double began_ = -1, firstFrame_ = -1, pressed_ = 0, next_ = 0, race_ = -1;
  uint32_t lastFrame_ = 0, raceFrame_ = 0;
  int held_ = -1;
  unsigned step_ = 0;
  bool cancel_ = false;
public:
  explicit RaceScenario(bool enabled,bool directRace=false) : enabled_(enabled),directRace_(directRace) {}
  unsigned steps()const{return directRace_?4:8;}
  bool enabled() const { return enabled_; }
  ScenarioState state() const { return state_; }
  unsigned step() const { return step_; }
  void cancel() { cancel_ = true; }
  ScenarioUpdate tick(double seconds, uint32_t frame, bool racing, bool viewReady=true) {
    ScenarioUpdate out;
    if (!enabled_ || state_ == ScenarioState::Ready || state_ == ScenarioState::Failed ||
        state_ == ScenarioState::Cancelled) return out;
    if (began_ < 0) began_ = seconds;
    auto release = [&] { out.release = held_; held_ = -1; };
    if (cancel_) {
      release();state_ = ScenarioState::Cancelled;out.cancelled = true;return out;
    }
    if (seconds - began_ >= 180) {
      release();state_ = ScenarioState::Failed;out.failed = true;out.capture = true;return out;
    }
    if (racing) {
      release();state_ = ScenarioState::AwaitingRace;
      if (race_ < 0) { race_ = seconds;raceFrame_ = frame; }
      if (seconds - race_ >= .5 && uint32_t(frame - raceFrame_) >= 3 && viewReady) {
        state_ = ScenarioState::Ready;out.ready = true;out.capture = true;
      }
      return out;
    }
    // Once physics is observed, never send more menu keys during a transition.
    if (race_ >= 0) { race_ = seconds;raceFrame_ = frame;return out; }
    if (held_ >= 0) {
      if (seconds - pressed_ >= .2) { release();out.capture = true; }
      return out;
    }
    if (frame && firstFrame_ < 0) firstFrame_ = seconds;
    if (firstFrame_ < 0 || frame < 60 || seconds - firstFrame_ < 1) return out;
    if (step_ >= steps()) { state_ = ScenarioState::AwaitingRace;return out; }
    if (seconds < next_ || (step_ && uint32_t(frame - lastFrame_) < 5)) return out;
    held_ = step_ == 6 ? 1 : 0;
    ++step_;pressed_ = seconds;next_ = seconds + 4;lastFrame_ = frame;
    state_ = ScenarioState::Navigating;out.press = held_;
    return out;
  }
};
inline const char *scenarioStateName(ScenarioState state) {
  switch (state) {
  case ScenarioState::Waiting: return "waiting";
  case ScenarioState::Navigating: return "navigating";
  case ScenarioState::AwaitingRace: return "awaiting-race";
  case ScenarioState::Ready: return "ready";
  case ScenarioState::Failed: return "failed";
  case ScenarioState::Cancelled: return "cancelled";
  }
  return "failed";
}
} // namespace fo2test
