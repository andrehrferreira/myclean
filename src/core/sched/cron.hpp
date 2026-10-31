#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::sched {

// A standard five-field cron expression: minute hour day-of-month month day-of-week.
//
// Each field is `*` or a comma-separated list of items `n`, `a-b`, `*/s`, `a-b/s` or `a/s` (from
// a to the field's maximum, every s). Day of week is 0-7, where 0 and 7 both mean Sunday. When
// both day fields are restricted (neither starts with `*`), a day matches if either field matches,
// as in Vixie cron. Month and weekday names are not accepted.
class CronExpression {
 public:
  // Fails with Errc::invalid_argument and a message that starts with the offending field's name,
  // e.g. "hour field: 25 is out of range 0-23".
  [[nodiscard]] static Result<CronExpression> parse(std::string_view text);

  [[nodiscard]] const std::string& text() const { return text_; }

  // The first occurrence strictly after `after`, in the local time of `zone`.
  // - A local time skipped by a daylight-saving jump (02:30 on a spring-forward night) occurs at
  //   the instant of the jump.
  // - A local time that occurs twice (02:30 on a fall-back night) occurs once, at its first
  //   instance; an occurrence whose first instance is not after `after` is passed.
  // Returns nullopt when the expression never matches a calendar date (`0 0 30 2 *`).
  [[nodiscard]] std::optional<WallTime> next_after(WallTime after,
                                                   const std::chrono::time_zone& zone) const;

 private:
  CronExpression() = default;

  [[nodiscard]] bool matches_day(std::chrono::local_days day) const;

  std::string text_;
  // One bit per allowed value, indexed minute, hour, day of month, month, day of week.
  std::array<std::uint64_t, 5> allowed_{};
  bool either_day_ = false;  // both day fields restricted: a day matches if either matches
};

}  // namespace myclean::sched
