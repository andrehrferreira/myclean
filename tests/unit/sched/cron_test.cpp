#include "core/sched/cron.hpp"

#include <chrono>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::sched {
namespace {

using namespace std::chrono_literals;

const std::chrono::time_zone& berlin() {
  return *std::chrono::locate_zone("Europe/Berlin");
}

WallTime local(std::chrono::year_month_day date, std::chrono::minutes time_of_day) {
  return berlin().to_sys(std::chrono::local_days{date} + time_of_day,
                         std::chrono::choose::earliest);
}

std::optional<WallTime> next(std::string_view expression, WallTime after) {
  const auto cron = CronExpression::parse(expression);
  EXPECT_TRUE(cron.has_value()) << expression;
  return cron ? cron->next_after(after, berlin()) : std::nullopt;
}

TEST(CronExpression, MonthlyExpression) {
  EXPECT_EQ(next("0 9 1 * *", local(2026y / 10 / 31, 12h)), local(2026y / 11 / 1, 9h));
}

TEST(CronExpression, StepsAndRangesSkipTheWeekend) {
  ASSERT_EQ(std::chrono::weekday{std::chrono::sys_days{2026y / 10 / 30}}, std::chrono::Friday);
  EXPECT_EQ(next("*/15 8-18 * * 1-5", local(2026y / 10 / 30, 18h + 50min)),
            local(2026y / 11 / 2, 8h));
}

TEST(CronExpression, ListsAndStepsFromAValue) {
  EXPECT_EQ(next("0,30 * * * *", local(2026y / 10 / 31, 10h + 10min)),
            local(2026y / 10 / 31, 10h + 30min));
  EXPECT_EQ(next("10/20 * * * *", local(2026y / 10 / 31, 10h + 31min)),
            local(2026y / 10 / 31, 10h + 50min));
  EXPECT_EQ(next("0 0-12/6 * * *", local(2026y / 10 / 31, 7h)), local(2026y / 10 / 31, 12h));
}

TEST(CronExpression, NextIsStrictlyAfter) {
  EXPECT_EQ(next("0 9 * * *", local(2026y / 10 / 31, 9h)), local(2026y / 11 / 1, 9h));
}

TEST(CronExpression, SevenAndZeroAreSunday) {
  EXPECT_EQ(next("0 0 * * 7", local(2026y / 10 / 30, 12h)), local(2026y / 11 / 1, 0h));
  EXPECT_EQ(next("0 0 * * 0", local(2026y / 10 / 30, 12h)), local(2026y / 11 / 1, 0h));
}

TEST(CronExpression, EitherDayFieldMatchesWhenBothAreRestricted) {
  // Friday 30 October: "the 13th or a Friday" next matches Friday 6 November ...
  EXPECT_EQ(next("0 0 13 * 5", local(2026y / 10 / 30, 12h)), local(2026y / 11 / 6, 0h));
  // ... and "the 2nd or a Friday" matches Monday 2 November.
  EXPECT_EQ(next("0 0 2 * 5", local(2026y / 10 / 30, 12h)), local(2026y / 11 / 2, 0h));
  // With an unrestricted day of week only the date counts.
  EXPECT_EQ(next("0 0 13 * *", local(2026y / 10 / 30, 12h)), local(2026y / 11 / 13, 0h));
}

TEST(CronExpression, InvalidHourRejectedNamingTheField) {
  const auto cron = CronExpression::parse("0 25 * * *");
  ASSERT_FALSE(cron.has_value());
  EXPECT_EQ(cron.error().code, Errc::invalid_argument);
  EXPECT_EQ(cron.error().message, "hour field: 25 is out of range 0-23");
}

TEST(CronExpression, InvalidExpressionsNameTheirField) {
  // Each expression with the start of its error message.
  const std::vector<std::pair<std::string_view, std::string_view>> cases{
      {"60 * * * *", "minute field"},   {"* * 0 * *", "day-of-month field"},
      {"* * * 13 *", "month field"},    {"* * * * 8", "day-of-week field"},
      {"*/0 * * * *", "minute field"},  {"* */25 * * *", "hour field"},
      {"5-3 * * * *", "minute field"},  {"1,,2 * * * *", "minute field"},
      {"x * * * *", "minute field"},    {"* * * jan *", "month field"},
      {"* * * *", "expected 5 fields"}, {"* * * * * *", "expected 5 fields"},
  };
  for (const auto& [expression, message_start] : cases) {
    const auto cron = CronExpression::parse(expression);
    ASSERT_FALSE(cron.has_value()) << expression;
    EXPECT_EQ(cron.error().code, Errc::invalid_argument);
    EXPECT_TRUE(cron.error().message.starts_with(message_start))
        << expression << ": " << cron.error().message;
  }
}

TEST(CronExpression, KeepsItsText) {
  const auto cron = CronExpression::parse("0  9 1 * *");
  ASSERT_TRUE(cron.has_value());
  EXPECT_EQ(cron->text(), "0  9 1 * *");
}

TEST(CronExpression, TimeSkippedBySpringForwardOccursAtTheJump) {
  // 29 March 2026: Berlin clocks jump from 02:00 to 03:00 (01:00 UTC); 02:30 does not exist.
  EXPECT_EQ(next("30 2 * * *", local(2026y / 3 / 29, 1h)),
            std::chrono::sys_days{2026y / 3 / 29} + 1h);
  EXPECT_EQ(next("30 2 * * *", std::chrono::sys_days{2026y / 3 / 29} + 1h),
            local(2026y / 3 / 30, 2h + 30min));
}

TEST(CronExpression, TimeRepeatedByFallBackOccursOnce) {
  // 25 October 2026: Berlin clocks go back from 03:00 to 02:00; 02:30 happens at 00:30 and
  // 01:30 UTC.
  const WallTime first = std::chrono::sys_days{2026y / 10 / 25} + 30min;
  EXPECT_EQ(next("30 2 * * *", local(2026y / 10 / 25, 0h)), first);
  EXPECT_EQ(next("30 2 * * *", first), local(2026y / 10 / 26, 2h + 30min));
  EXPECT_EQ(next("30 2 * * *", first + 50min), local(2026y / 10 / 26, 2h + 30min));
}

TEST(CronExpression, LeapDayAndImpossibleDate) {
  EXPECT_EQ(next("0 0 29 2 *", local(2026y / 10 / 31, 0h)), local(2028y / 2 / 29, 0h));
  EXPECT_EQ(next("0 0 30 2 *", local(2026y / 10 / 31, 0h)), std::nullopt);
}

}  // namespace
}  // namespace myclean::sched
