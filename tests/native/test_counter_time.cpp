#include "CounterTime.h"

#include <assert.h>
#include <stdint.h>

#include <iostream>

using pixelos::counterForwardDelta;
using pixelos::counterProjectedElapsed;
using pixelos::counterTimestampAtOrAfter;

namespace {

void testStaleCrossTaskTimestampDoesNotBecomeFortyNineDays() {
  constexpr uint32_t backgroundTimestamp = 1001;
  constexpr uint32_t earlierDisplayTimestamp = 1000;
  static_assert(!counterTimestampAtOrAfter(earlierDisplayTimestamp,
                                           backgroundTimestamp),
                "stale timestamp must be rejected");
  static_assert(counterForwardDelta(earlierDisplayTimestamp,
                                    backgroundTimestamp) == 0,
                "stale timestamp must not underflow");
  static_assert(counterProjectedElapsed(12345, true, true,
                                        backgroundTimestamp,
                                        earlierDisplayTimestamp) == 12345,
                "stale render sample must not jump the counter");
}

void testNormalForwardProgress() {
  assert(counterForwardDelta(1350, 1000) == 350);
  assert(counterProjectedElapsed(2000, true, true, 1000, 1350) == 2350);
  assert(counterProjectedElapsed(2000, false, true, 1000, 1350) == 2000);
}

void testRealMillisRolloverStillAdvances() {
  constexpr uint32_t beforeWrap = 0xfffffff0U;
  constexpr uint32_t afterWrap = 0x00000020U;
  static_assert(counterTimestampAtOrAfter(afterWrap, beforeWrap),
                "nearby rollover is forward progress");
  static_assert(counterForwardDelta(afterWrap, beforeWrap) == 48U,
                "rollover delta must remain exact");
}

void testZeroIsAValidInitializedTimestamp() {
  static_assert(counterProjectedElapsed(0, true, true, 0, 25) == 25,
                "a counter started during the first millisecond must advance");
  static_assert(counterProjectedElapsed(0, true, false, 0, 25) == 0,
                "an uninitialized timestamp must not advance");
}

}  // namespace

int main() {
  testStaleCrossTaskTimestampDoesNotBecomeFortyNineDays();
  testNormalForwardProgress();
  testRealMillisRolloverStillAdvances();
  testZeroIsAValidInitializedTimestamp();
  std::cout << "CounterTime native tests passed\n";
  return 0;
}
