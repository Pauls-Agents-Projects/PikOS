#pragma once

#include <stdint.h>

namespace pixelos {

// `millis()` wraps every 49.7 days.  Subtracting remains rollover-safe as
// long as timestamps are observed in order, but PikOS's display and main
// tasks can capture adjacent values and acquire the AppEngine lock in the
// opposite order.  Treat a negative signed distance as a stale observation
// rather than turning a one-millisecond inversion into a 49.7-day interval.
constexpr bool counterTimestampAtOrAfter(uint32_t candidate,
                                         uint32_t reference) {
  return static_cast<int32_t>(candidate - reference) >= 0;
}

constexpr uint32_t counterForwardDelta(uint32_t candidate,
                                       uint32_t reference) {
  return counterTimestampAtOrAfter(candidate, reference)
             ? static_cast<uint32_t>(candidate - reference)
             : 0U;
}

constexpr uint64_t counterProjectedElapsed(uint64_t accumulatedMs,
                                           bool running,
                                           bool timestampInitialized,
                                           uint32_t lastUpdateMs,
                                           uint32_t nowMs) {
  return accumulatedMs +
         (running && timestampInitialized
              ? counterForwardDelta(nowMs, lastUpdateMs)
              : 0U);
}

}  // namespace pixelos
