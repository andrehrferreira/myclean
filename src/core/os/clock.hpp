#pragma once

#include <chrono>

#include "core/model/units.hpp"

namespace myclean::os {

// The source of time for core: injected so that schedules, batching and retention are tested
// without waiting.
class IClock {
 public:
  virtual ~IClock() = default;
  IClock(const IClock&) = delete;
  IClock& operator=(const IClock&) = delete;
  IClock(IClock&&) = delete;
  IClock& operator=(IClock&&) = delete;

  // Wall-clock time; jumps when the clock is changed.
  [[nodiscard]] virtual WallTime now() const = 0;
  // Monotonic time; never jumps.
  [[nodiscard]] virtual MonoTime monotonic() const = 0;
  // The local time zone, for computing local times such as schedule occurrences.
  [[nodiscard]] virtual const std::chrono::time_zone& zone() const = 0;

 protected:
  IClock() = default;
};

// The real clock: std::chrono system and steady clocks and the machine's current time zone.
class SystemClock final : public IClock {
 public:
  SystemClock() = default;

  [[nodiscard]] WallTime now() const override;
  [[nodiscard]] MonoTime monotonic() const override;
  [[nodiscard]] const std::chrono::time_zone& zone() const override;
};

}  // namespace myclean::os
