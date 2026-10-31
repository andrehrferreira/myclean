#include "fakes/fake_clock.hpp"

#include <chrono>

#include <gtest/gtest.h>

#include "core/model/units.hpp"

namespace myclean::fakes {
namespace {

using namespace std::chrono_literals;

constexpr WallTime start = std::chrono::sys_days{std::chrono::year{2026} / 10 / 31} + 12h;

TEST(FakeClock, AdvanceMovesBothClocks) {
  FakeClock clock{start};
  const MonoTime mono_start = clock.monotonic();
  clock.advance(90min);
  EXPECT_EQ(clock.now(), start + 90min);
  EXPECT_EQ(clock.monotonic() - mono_start, 90min);
}

TEST(FakeClock, WallClockChangeLeavesMonotonicTime) {
  FakeClock clock{start};
  const MonoTime mono_start = clock.monotonic();
  clock.set_wall(start - 1h);
  EXPECT_EQ(clock.now(), start - 1h);
  EXPECT_EQ(clock.monotonic(), mono_start);
}

TEST(FakeClock, ZoneGivesLocalTimeIncludingDaylightSaving) {
  const FakeClock clock{start, "Europe/Berlin"};
  EXPECT_EQ(clock.zone().name(), "Europe/Berlin");
  // 2026-10-25 01:00 UTC is after the switch back to CET (UTC+1).
  const WallTime after_switch = std::chrono::sys_days{std::chrono::year{2026} / 10 / 25} + 1h;
  EXPECT_EQ(clock.zone().get_info(after_switch).offset, 1h);
  EXPECT_EQ(clock.zone().get_info(after_switch - 2h).offset, 2h);
}

}  // namespace
}  // namespace myclean::fakes
