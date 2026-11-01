#pragma once

#include <chrono>
#include <cstdint>

#include "core/events/events.hpp"
#include "core/governor/forest.hpp"
#include "core/model/units.hpp"
#include "core/os/process_control.hpp"

// Conversions at the boundary between the governor's decisions (core/governor), the OS interfaces
// (core/os) and the events, which each name a process instance with their own ProcessKey.
namespace myclean::governing {

[[nodiscard]] inline os::ProcessKey to_os(const governor::ProcessKey& key) {
  return {.pid = key.pid, .created = key.created};
}

[[nodiscard]] inline governor::ProcessKey to_governor(const os::ProcessKey& key) {
  return {.pid = key.pid, .created = key.created};
}

[[nodiscard]] inline governor::ProcessKey to_governor(const events::ProcessKey& key) {
  return {.pid = key.pid, .created = key.created};
}

// Creation times are stored and cross the pipe as nanoseconds since the Unix epoch, which keeps
// the 100 ns ticks of Windows exact, so a key read back names the same process instance.
[[nodiscard]] inline std::int64_t to_nanoseconds(WallTime time) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}

[[nodiscard]] inline WallTime from_nanoseconds(std::int64_t nanoseconds) {
  return WallTime{
      std::chrono::duration_cast<WallTime::duration>(std::chrono::nanoseconds{nanoseconds})};
}

}  // namespace myclean::governing
