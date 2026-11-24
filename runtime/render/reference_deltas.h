#pragma once
#include <initializer_list>
namespace fo2render {
// Acquire pending ownership before any release can destroy a parent through
// its last child. The queue must preserve this order as well as bind order.
template<class Records, class Emit>
void flushReferenceDeltas(Records &dirty, Emit emit) {
  for (bool positive : {true, false})
    for (auto *record : dirty) {
      const auto delta = record->refDelta;
      if (delta && (delta > 0) == positive) {
        emit(record->native, delta);
        record->refDelta = 0;
      }
    }
  for (auto *record : dirty) record->refQueued = false;
  dirty.clear();
}
} // namespace fo2render
