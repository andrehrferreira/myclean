#include "core/os/clock.hpp"

#include <chrono>

#include <gtest/gtest.h>

#include "core/model/units.hpp"

namespace myclean::os {
namespace {

TEST(SystemClock, MonotonicTimeNeverGoesBack) {
  const SystemClock clock;
  const MonoTime first = clock.monotonic();
  const MonoTime second = clock.monotonic();
  EXPECT_LE(first, second);
}

TEST(SystemClock, WallTimeIsCurrentAndZoneIsKnown) {
  const SystemClock clock;
  const WallTime before = std::chrono::system_clock::now();
  const WallTime now = clock.now();
  EXPECT_LE(before, now);
  EXPECT_FALSE(clock.zone().name().empty());
}

}  // namespace
}  // namespace myclean::os
