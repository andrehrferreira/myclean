#pragma once

#include <chrono>
#include <string_view>

#include "core/model/units.hpp"
#include "core/os/clock.hpp"

namespace myclean::fakes {

// A clock that moves only when a test moves it.
class FakeClock final : public os::IClock {
 public:
  // `zone` is an IANA name such as "Europe/Berlin"; an unknown name throws.
  explicit FakeClock(WallTime start, std::string_view zone = "UTC");

  [[nodiscard]] WallTime now() const override { return wall_; }
  [[nodiscard]] MonoTime monotonic() const override { return monotonic_; }
  [[nodiscard]] const std::chrono::time_zone& zone() const override { return *zone_; }

  // Time passes: both clocks move forward by `duration`.
  void advance(std::chrono::nanoseconds duration);
  // Someone changes the wall clock: only wall time jumps, monotonic time does not.
  void set_wall(WallTime time) { wall_ = time; }

 private:
  WallTime wall_;
  MonoTime monotonic_;
  const std::chrono::time_zone* zone_;
};

}  // namespace myclean::fakes
