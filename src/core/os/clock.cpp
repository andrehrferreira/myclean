#include "core/os/clock.hpp"

#include <chrono>

#include "core/model/units.hpp"

namespace myclean::os {

WallTime SystemClock::now() const {
  return std::chrono::system_clock::now();
}

MonoTime SystemClock::monotonic() const {
  return std::chrono::steady_clock::now();
}

const std::chrono::time_zone& SystemClock::zone() const {
  return *std::chrono::current_zone();
}

}  // namespace myclean::os
