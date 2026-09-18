#include "GarminActivityUtils.h"

#include <assert.h>
#include <stdint.h>

#include <iostream>
#include <limits>

using pixelos::GarminActivityType;
using pixelos::garminActivityTypeFromKey;
using pixelos::garminIsoUtcEpoch;
using pixelos::garminRoundedUnsigned;
using pixelos::garminUtcEpoch;

namespace {

void testActivityClassification() {
  assert(garminActivityTypeFromKey("running") ==
         GarminActivityType::Running);
  assert(garminActivityTypeFromKey("TRAIL_RUNNING") ==
         GarminActivityType::Running);
  assert(garminActivityTypeFromKey("road_biking") ==
         GarminActivityType::Cycling);
  assert(garminActivityTypeFromKey("HIKING") ==
         GarminActivityType::Walking);
  assert(garminActivityTypeFromKey("lap_swimming") ==
         GarminActivityType::Swimming);
  assert(garminActivityTypeFromKey("strength_training") ==
         GarminActivityType::Other);
  assert(garminActivityTypeFromKey(nullptr) == GarminActivityType::Other);
}

void testUtcCivilConversionWithoutTimegm() {
  assert(garminUtcEpoch(1970, 1, 2, 0, 0, 0) == 86400U);
  assert(garminUtcEpoch(2021, 1, 1, 0, 0, 0) == 1609459200U);
  assert(garminIsoUtcEpoch("2024-02-29T00:00:00.000") == 1709164800U);
  assert(garminIsoUtcEpoch("2021-01-01 00:00:00") == 1609459200U);
  assert(garminIsoUtcEpoch("2023-02-29T00:00:00") == 0U);
  assert(garminIsoUtcEpoch("2024/02/29 00:00:00") == 0U);
  assert(garminIsoUtcEpoch(nullptr) == 0U);
}

void testProviderNumberBounds() {
  assert(garminRoundedUnsigned(-1.0) == 0U);
  assert(garminRoundedUnsigned(0.49) == 0U);
  assert(garminRoundedUnsigned(0.5) == 1U);
  assert(garminRoundedUnsigned(1234.6) == 1235U);
  assert(garminRoundedUnsigned(70000.0, UINT16_MAX) == UINT16_MAX);
  assert(garminRoundedUnsigned(std::numeric_limits<double>::infinity()) ==
         0U);
  assert(garminRoundedUnsigned(std::numeric_limits<double>::quiet_NaN()) ==
         0U);
}

}  // namespace

int main() {
  testActivityClassification();
  testUtcCivilConversionWithoutTimegm();
  testProviderNumberBounds();
  std::cout << "Garmin activity utility tests passed\n";
  return 0;
}
