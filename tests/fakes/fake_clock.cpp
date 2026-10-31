#include "fakes/fake_clock.hpp"

#include <chrono>
#include <string_view>

#include "core/model/units.hpp"

namespace myclean::fakes {

FakeClock::FakeClock(WallTime start, std::string_view zone)
    : wall_(start), zone_(std::chrono::locate_zone(zone)) {}

void FakeClock::advance(std::chrono::nanoseconds duration) {
  wall_ += std::chrono::duration_cast<WallTime::duration>(duration);
  monotonic_ += std::chrono::duration_cast<MonoTime::duration>(duration);
}

}  // namespace myclean::fakes
